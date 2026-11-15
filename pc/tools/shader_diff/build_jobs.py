#!/usr/bin/env python3
"""Builds the jobs of the shader differential test.

For every shader of a DXVK warm file there are two translations of the same D3D9 bytecode:
  swarc: SwARc's GLSL (`translate` wrote it) -> glslangValidator -> SPIR-V
  dxvk:  the SPIR-V DXVK itself dumped (DXVK_SHADER_DUMP_PATH), with the sampler heap made a 1-element array
Both go through `spirv-cross --msl`. This script reads the two MSL sources and SPIRV-Cross's reflection, appends a
small driver to each (pixel shaders: a vertex function that feeds the varyings; vertex shaders: a compute kernel
that calls the vertex function and stores its outputs), and writes the input data: the same constant registers,
varyings / vertex attributes and textures for both sides, matched through D3D9 register numbers and semantics.

Output (all under <work>, never in the repo: it is derived from game shaders):
  jobs.jsonl          one job per shader, read by `runner`
  job/<name>.<side>.metal, .bin
  textures.bin        the shared random textures
"""
import argparse
import hashlib
import json
import multiprocessing
import os
import random
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm3  # noqa: E402
import spvpatch  # noqa: E402

SETS = 4          # input sets per shader: benign, signed, edge, zero
PS_W = PS_H = 8   # pixels per set
VS_N = 16         # vertices per set
VS_SLOTS = 40     # float4 per vertex in the kernel's output: 0 position, 1+location, 34 point size
ALIGN = 256

USAGE_ID = {'position': 0, 'weight': 1, 'blend': 2, 'normal': 3, 'psize': 4, 'texcoord': 5, 'tangent': 6,
            'binormal': 7, 'tessfactor': 8, 'position_t': 9, 'color': 10, 'fog': 11, 'depth': 12, 'sample': 13}
SM3_USAGE_ID = {'position': 0, 'blendweight': 1, 'blendindices': 2, 'normal': 3, 'psize': 4, 'texcoord': 5,
                'tangent': 6, 'binormal': 7, 'tessfactor': 8, 'positiont': 9, 'color': 10, 'fog': 11, 'depth': 12,
                'sample': 13}


def swarc_semantic(loc):
    """Inverse of SwARc's VaryingLocation (hlsl.cpp)."""
    if loc < 16:
        return (5, loc)
    table = {16: (10, 0), 17: (10, 1), 18: (11, 0), 19: (3, 0), 20: (3, 1), 21: (6, 0), 22: (7, 0), 23: (1, 0),
             24: (2, 0), 25: (0, 1), 26: (0, 2), 27: (0, 3), 28: (4, 0), 29: (12, 0)}
    return table[loc]


def dxvk_semantic(name):
    m = re.match(r'^[vo]\d+_([a-z_]+?)(\d*)$', name)
    if m and m.group(1) == 'specular':   # DXVK's name for COLOR1+
        return (10, 1 + int(m.group(2) or 0))
    if not m or m.group(1) not in USAGE_ID:
        raise ValueError('DXVK interface name not understood: ' + name)
    return (USAGE_ID[m.group(1)], int(m.group(2) or 0))


# ---- input values: a function of (shader, set, register or semantic) only, so both sides get the same

class Values:
    def __init__(self, name, shader):
        self.name, self.sh = name, shader

    def rng(self, *key):
        h = hashlib.md5(repr((self.name,) + key).encode()).digest()
        return random.Random(h)

    def const(self, s, reg):
        if reg in self.sh.defs:   # what Direct3D would hold there is irrelevant: the shader's own value wins
            return list(self.sh.defs[reg])
        r = self.rng('c', s, reg)
        if s == 3:
            return [0.0] * 4
        if s == 0:
            return [r.uniform(0.25, 1.5) for _ in range(4)]
        if s == 1:
            return [r.uniform(-2.0, 2.0) for _ in range(4)]
        return [r.choice((0.0, 0.0, 1.0, -1.0, 0.5, 2.0, r.uniform(-2.0, 2.0), r.uniform(-2.0, 2.0)))
                for _ in range(4)]

    def vary(self, s, sem, px, py):
        r = self.rng('v', s, sem)
        if s == 3:
            return [0.0] * 4
        if s == 0:
            base = [r.uniform(0.2, 0.9) for _ in range(4)]
            g = [[r.uniform(-0.03, 0.03) for _ in range(4)] for _ in range(2)]
        elif s == 1:
            base = [r.uniform(-1.0, 1.0) for _ in range(4)]
            g = [[r.uniform(-0.05, 0.05) for _ in range(4)] for _ in range(2)]
        else:
            base = [r.choice((0.0, 0.0, 1.0, r.uniform(-1.0, 1.0))) for _ in range(4)]
            g = [[r.choice((0.0, 0.0, r.uniform(-0.05, 0.05))) for _ in range(4)] for _ in range(2)]
        return [base[k] + g[0][k] * px + g[1][k] * py for k in range(4)]

    def attr(self, s, sem, v):
        r = self.rng('a', s, sem, v)
        if s == 3:
            return [0.0] * 4
        if sem[0] == 2:   # BLENDINDICES: bone numbers
            return [float(r.randrange(0, 60)) if s < 2 else r.uniform(0.0, 60.0) for _ in range(4)]
        if s == 0:
            return [r.uniform(0.1, 1.0) for _ in range(4)]
        if s == 1:
            return [r.uniform(-1.0, 1.0) for _ in range(4)]
        return [r.choice((0.0, 0.0, 1.0, -1.0, r.uniform(-1.0, 1.0))) for _ in range(4)]


# ---- MSL reading

def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<(':
            depth += 1
        elif ch in '>)':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


PARAM = re.compile(r'^(.*?)\s*\b(\w+)\s*\[\[(\w+)(?:\((\w+)\))?\]\]$')
MEMBER = re.compile(r'^\s*(\w+)\s+(\w+)\s*\[\[([^\]]*)\]\](\s*\[\d+\])?;', re.M)


def entry_params(msl, kind):
    m = re.search(r'^%s (\w+) main0\((.*)\)$' % kind, msl, re.M)
    if not m:
        raise ValueError('no %s entry point' % kind)
    params = []
    for p in split_params(m.group(2)):
        pm = PARAM.match(p)
        if not pm:
            raise ValueError('entry parameter not understood: ' + p)
        ty = re.sub(r'^(constant|const|thread|device)\s+', '', pm.group(1)).strip()
        params.append({'text': p, 'type': ty, 'name': pm.group(2), 'attr': pm.group(3), 'arg': pm.group(4)})
    return m, params


def struct_members(msl, name):
    m = re.search(r'^struct %s\n\{\n(.*?)\n\};' % name, msl, re.M | re.S)
    if not m:
        return []
    return [{'type': t, 'name': n, 'attr': a, 'array': bool(arr)} for t, n, a, arr in MEMBER.findall(m.group(1))]


SWZ = {'float4': '', 'float3': '.xyz', 'float2': '.xy', 'float': '.x'}
PAD = {'float4': '%s', 'float3': 'float4(%s, 0.0)', 'float2': 'float4(%s, 0.0, 0.0)',
       'float': 'float4(%s, 0.0, 0.0, 0.0)'}


def locn(attr):
    m = re.search(r'user\(locn(\d+)\)', attr)
    return int(m.group(1)) if m else None


# ---- per side

class Blob:
    def __init__(self):
        self.b = bytearray()

    def add(self, data, size=0):
        off = len(self.b)
        self.b += data
        n = max(len(data), size) + ALIGN   # slack: Metal reads the whole struct
        self.b += bytes(off + n - len(self.b))
        self.b += bytes(-len(self.b) % ALIGN)
        return off


def fill_block(side, block_name, refl, vals, s):
    """Bytes of one uniform block / push-constant block for input set s."""
    ty = None
    for group in ('ubos', 'push_constants'):
        for u in refl.get(group, []):
            if refl['types'][u['type']]['name'] == block_name:
                ty = refl['types'][u['type']]
    if ty is None:
        raise ValueError('block %s not in reflection' % block_name)
    size = 0
    parts = []
    known = block_name in ('specData_buf', 'commonState_buf', 'clipPlanes_buf', 'push_data_t')
    for mem in ty['members']:
        n, off, mty = mem['name'], mem['offset'], mem['type']
        count = mem['array'][0] if mem.get('array') else 1
        data = None
        if mty == 'vec4':
            cm = re.match(r'^c(\d+)(_|$)', n)
            if side == 'swarc' and block_name == 'r_float_regs' and n == 'c':
                data = b''.join(struct.pack('<4f', *vals.const(s, i)) for i in range(count))
            elif side == 'dxvk' and block_name == 'c_buf' and cm:
                data = b''.join(struct.pack('<4f', *vals.const(s, int(cm.group(1)) + i)) for i in range(count))
            elif side == 'dxvk' and n == 'm' and re.match(r'^c(\d+)_.*_buf$', block_name):
                # a buffer of one register is named after it
                first = int(re.match(r'^c(\d+)_', block_name).group(1))
                data = b''.join(struct.pack('<4f', *vals.const(s, first + i)) for i in range(count))
            elif side == 'dxvk' and block_name == 'cF_buf' and n == 'm':
                data = b''.join(struct.pack('<4f', *(vals.const(s, i) if i < 256 else [0.0] * 4))
                                for i in range(count))
            elif block_name == 'clipPlanes_buf':
                data = bytes(16 * count)
            size = max(size, off + 16 * count)
        elif mty == 'ivec4':
            # i registers (loop counts): 4 iterations from 0 step 1; b registers: true
            data = struct.pack('<4i', 4, 0, 1, 0) * count if n != 'r_b' else struct.pack('<4i', 1, 1, 1, 1) * count
            size = max(size, off + 16 * count)
        elif n in ('commonState', 'm') and block_name in ('specData_buf', 'commonState_buf') and off == 0:
            data = struct.pack('<I', 7)    # alpha test: always pass; no clip planes, no point sprites
        elif n == 'cFSize':
            data = struct.pack('<H', 256)  # float constants the shader may index: all of them
        elif known:
            data = b''
        if data is None:
            raise ValueError('uniform %s.%s (%s) has no rule' % (block_name, n, mty))
        parts.append((off, data))
        size = max(size, off + len(data), off + 16)
    buf = bytearray(size)
    for off, data in parts:
        buf[off:off + len(data)] = data
    return bytes(buf)


def resources(side, params, refl, vals, blob):
    bufs, texs, smps = [], [], []
    for p in params:
        if p['attr'] == 'buffer':
            block = p['type'].rstrip('&').strip()
            offs = [blob.add(fill_block(side, block, refl, vals, s)) for s in range(SETS)]
            bufs.append({'index': int(p['arg']), 'offs': offs})
        elif p['attr'] == 'texture':
            m = re.match(r'^r_s(\d+)$', p['name']) if side == 'swarc' else re.match(r'^s(\d+)_', p['name'])
            kind = {'texture2d<float>': '2d', 'texturecube<float>': 'cube', 'texture3d<float>': '3d'}.get(p['type'])
            if not m or not kind:
                raise ValueError('texture parameter not understood: ' + p['text'])
            texs.append({'index': int(p['arg']), 'kind': kind, 'reg': int(m.group(1))})
        elif p['attr'] == 'sampler':
            smps.append(int(p['arg']))
    return bufs, texs, smps


def build_ps(side, msl, refl, vals, semantic_of, blob):
    _, params = entry_params(msl, 'fragment')
    vin = [m for m in struct_members(msl, 'main0_in') if locn(m['attr']) is not None]
    for m in vin:
        if m['type'] not in SWZ or m['array']:
            raise ValueError('varying type not handled: ' + m['type'])
    drv = '\nstruct sd_vs_out\n{\n    float4 sd_pos [[position]];\n'
    for i, m in enumerate(vin):
        drv += '    %s m%d [[user(locn%d)]];\n' % (m['type'], i, locn(m['attr']))
    drv += '};\n\nvertex sd_vs_out sd_vs(uint vid [[vertex_id]], const device float4* vary [[buffer(0)]])\n{\n'
    drv += '    sd_vs_out o;\n    o.sd_pos = float4(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0, 0.5, 1.0);\n'
    nv = max(len(vin), 1)
    for i, m in enumerate(vin):
        drv += '    o.m%d = vary[vid * %d + %d]%s;\n' % (i, nv, i, SWZ[m['type']])
    drv += '    return o;\n}\n'
    # Pixel positions of the three vertices (y down): NDC (-1,-1) is the bottom left.
    vpos = [(0.0, float(PS_H)), (2.0 * PS_W, float(PS_H)), (0.0, -float(PS_H))]
    sems = [semantic_of(m) for m in vin]
    vary_offs = []
    for s in range(SETS):
        data = b''
        for px, py in vpos:
            for sem in sems:
                data += struct.pack('<4f', *vals.vary(s, sem, px, py))
            if not sems:
                data += bytes(16)
        vary_offs.append(blob.add(data))
    bufs, texs, smps = resources(side, params, refl, vals, blob)
    outs = struct_members(msl, 'main0_out')
    colors = sorted(int(re.search(r'color\((\d+)\)', m['attr']).group(1)) for m in outs if 'color(' in m['attr'])
    depth = any(m['attr'].startswith('depth') for m in outs)
    return {'msl': msl + drv, 'io': vary_offs, 'buffers': bufs, 'textures': texs, 'samplers': smps,
            'colors': colors, 'depth': depth, 'varyings': sorted(sems)}


def build_vs(side, msl, refl, vals, in_semantic_of, out_semantic_of, blob):
    em, params = entry_params(msl, 'vertex')
    ret = em.group(1)
    line = em.group(0)
    impl = re.sub(r'\s*\[\[[^\]]*\]\]', '', line).replace('vertex ', 'static ', 1).replace(' main0(', ' sd_main0(', 1)
    msl = msl.replace(line, impl, 1)
    vin = [m for m in struct_members(msl, 'main0_in')]
    kparams, args, pre = [], [], ''
    for p in params:
        if p['attr'] == 'stage_in':
            pre += '    main0_in sd_i = {};\n'
            args.append('sd_i')
        elif p['attr'] in ('buffer', 'texture', 'sampler'):
            kparams.append(p['text'])
            args.append(p['name'])
        elif p['attr'] == 'vertex_id':
            args.append('sd_tid.x')
        elif p['attr'] in ('instance_id', 'base_vertex', 'base_instance'):
            args.append('0u')
        else:
            raise ValueError('vertex entry parameter not handled: ' + p['text'])
    sems = []
    for m in vin:
        am = re.match(r'attribute\((\d+)\)', m['attr'])
        if not am or m['type'] not in SWZ:
            raise ValueError('vertex input not handled: %s %s' % (m['type'], m['attr']))
        pre += '    sd_i.%s = sd_in[sd_tid.x * 16 + %d]%s;\n' % (m['name'], len(sems), SWZ[m['type']])
        sems.append(in_semantic_of(m, int(am.group(1))))
    post = ''
    outmap = {}
    if ret != 'void':
        for m in struct_members(msl, ret):
            if m['array'] or m['type'] not in PAD:
                continue
            if m['attr'].split(',')[0] == 'position':
                slot, key = 0, 'position'
            elif m['attr'] == 'point_size':
                slot, key = 34, 'psize'
            elif locn(m['attr']) is not None:
                slot = 1 + locn(m['attr'])
                key = '%d.%d' % out_semantic_of(m)
            else:
                continue
            outmap[key] = slot
            post += '    sd_p[%d] = %s;\n' % (slot, PAD[m['type']] % ('sd_o.' + m['name']))
    drv = '\nkernel void sd_wrap(uint3 sd_tid [[thread_position_in_grid]], const device float4* sd_in [[buffer(29)]], '
    drv += 'device float4* sd_out [[buffer(30)]]' + ''.join(', ' + k for k in kparams) + ')\n{\n' + pre
    drv += '    device float4* sd_p = sd_out + sd_tid.x * %d;\n' % VS_SLOTS
    drv += '    for (int k = 0; k < %d; k++) sd_p[k] = float4(-7777.0);\n' % VS_SLOTS
    if ret != 'void':
        drv += '    %s sd_o = sd_main0(%s);\n' % (ret, ', '.join(args))
    drv += post + '}\n'
    in_offs = []
    for s in range(SETS):
        data = b''
        for v in range(VS_N):
            row = b''.join(struct.pack('<4f', *vals.attr(s, sem, v)) for sem in sems)
            data += row + bytes(16 * 16 - len(row))
        in_offs.append(blob.add(data))
    bufs, texs, smps = resources(side, params, refl, vals, blob)
    return {'msl': msl + drv, 'io': in_offs, 'buffers': bufs, 'textures': texs, 'samplers': smps, 'outputs': outmap,
            'inputs': sorted(sems)}


FIX_HELPERS = """
const float SD_FLT_MAX = 3.402823466e+38;
float sd_rcp(float x) { return x == 0.0 ? SD_FLT_MAX : 1.0 / x; }
float sd_rsq(float x) { return x == 0.0 ? SD_FLT_MAX : inversesqrt(abs(x)); }
float sd_nrm(float d) { return d == 0.0 ? SD_FLT_MAX : inversesqrt(d); }
float sd_log(float x) { return x == 0.0 ? -SD_FLT_MAX : log2(abs(x)); }
float sd_pow(float a, float b) { return min(exp2(sd_log(a) * b), SD_FLT_MAX); }
"""


def close_call(text, start):
    """Index just past the parenthesis that closes the one opened right before `start`."""
    depth, i = 1, start
    while depth:
        depth += {'(': 1, ')': -1}.get(text[i], 0)
        i += 1
    return i


def apply_fixes(glsl, fixes):
    """Experiment only: rewrites the forms SwARc emits for rcp/rsq/nrm/log/pow to the results the Direct3D 9
    reference gives for a zero operand (FLT_MAX instead of Inf/NaN). Used to attribute differences to a cause."""
    if not fixes:
        return glsl
    head, sep, body = glsl.partition('// ')
    body = sep + body
    if 'rcp' in fixes:
        out, pos = '', 0
        while True:
            i = body.find('vec4(1.0 / ', pos)
            if i < 0:
                break
            a = i + len('vec4(1.0 / ')
            e = close_call(body, a)
            out += body[pos:i] + 'vec4(sd_rcp(' + body[a:e - 1] + '))'
            pos = e
        body = out + body[pos:]
    if 'rsq' in fixes:
        body = body.replace('vec4(inversesqrt(abs(', 'vec4(sd_rsq((')
    if 'nrm' in fixes:
        body = body.replace(' * inversesqrt(dot(', ' * sd_nrm(dot(')
    if 'log' in fixes:
        body = body.replace('vec4(log2(abs(', 'vec4(sd_log((')
    if 'pow' in fixes:
        body = body.replace('vec4(pow(abs(', 'vec4(sd_pow((')
    return head + FIX_HELPERS + body


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise ValueError('%s: %s' % (os.path.basename(cmd[0]), (r.stdout + r.stderr).strip()[:300]))
    return r.stdout


G = {}


def prepare(rec):
    name, px, code = rec
    w = G['work']
    try:
        sh = sm3.Shader(code)
        vals = Values(name, sh)
        ext = 'frag' if px else 'vert'
        tmp = os.path.join(w, 'spv')
        sides = {}
        # SwARc
        s_spv = os.path.join(tmp, name + '.swarc.spv')
        src = os.path.join(w, 'swarc', name + '.' + ext)
        if G['fixes']:
            with open(src) as f:
                fixed = apply_fixes(f.read(), G['fixes'])
            src = os.path.join(tmp, name + '.fixed.' + ext)
            with open(src, 'w') as f:
                f.write(fixed)
        run(['glslangValidator', '-V', '-S', ext, src, '-o', s_spv])
        # DXVK
        d_spv = os.path.join(tmp, name + '.dxvk.spv')
        with open(os.path.join(G['dump'], name + '.spv'), 'rb') as f:
            patched = spvpatch.patch(f.read())
        with open(d_spv, 'wb') as f:
            f.write(patched)
        meta = G['meta'][name]
        job = {'name': name, 'stage': 'ps' if px else 'vs', 'sides': {}}
        for side, spv in (('swarc', s_spv), ('dxvk', d_spv)):
            msl = run(['spirv-cross', spv, '--msl', '--msl-version', '20300'])
            refl = json.loads(run(['spirv-cross', spv, '--reflect']))
            blob = Blob()
            if px:
                if side == 'swarc':
                    sem = lambda m: swarc_semantic(locn(m['attr']))
                else:
                    sem = lambda m: dxvk_semantic(m['name'])
                d = build_ps(side, msl, refl, vals, sem, blob)
            else:
                if side == 'swarc':
                    isem = lambda m, a: tuple(meta['inputs'][a])
                    osem = lambda m: swarc_semantic(locn(m['attr']))
                else:
                    isem = lambda m, a: dxvk_semantic(m['name'])
                    osem = lambda m: dxvk_semantic(m['name'])
                d = build_vs(side, msl, refl, vals, isem, osem, blob)
            base = os.path.join(w, 'job', name + '.' + side)
            with open(base + '.metal', 'w') as f:
                f.write(d.pop('msl'))
            with open(base + '.bin', 'wb') as f:
                f.write(bytes(blob.b))
            d['msl'], d['bin'], d['out'] = base + '.metal', base + '.bin', os.path.join(w, 'out', name + '.' + side)
            job['sides'][side] = d
        # What Direct3D declares, for the comparison
        if not px:
            job['dcl_out'] = [[SM3_USAGE_ID[u], i, m or 15] for _, u, i, m in sh.outputs]
        job['features'] = sorted(sh.features | {'smp:' + v for v in sh.samplers.values()})
        job['size'] = len(code)
        return job
    except Exception as e:  # noqa: BLE001
        return {'name': name, 'error': '%s: %s' % (type(e).__name__, e)}


def write_textures(path):
    """16 registers x (2D 16x16, cube 8x8, 3D 8x8x8), every mip level its own random RGBA16F values in [0, 1]."""
    r = random.Random(20261115)
    out = bytearray()
    for kind, size, layers in (('2d', 16, 1), ('cube', 8, 6), ('3d', 8, 0)):
        for reg in range(16):
            s = size
            while s >= 1:
                n = s * s * (layers if layers else s)
                out += struct.pack('<%de' % (4 * n), *[r.random() for _ in range(4 * n)])
                s //= 2
    with open(path, 'wb') as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--shaders', required=True, help='pipelines.warm.shaders')
    ap.add_argument('--dump', required=True, help='directory of DXVK_SHADER_DUMP_PATH (.spv per shader)')
    ap.add_argument('--work', required=True, help='work directory (has swarc/ from `translate`)')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--only', help='file of shader names (one per line): build just these')
    ap.add_argument('--swarc-fix', default='', help='experiment: comma list of rcp,rsq,nrm,log,pow (see apply_fixes)')
    a = ap.parse_args()
    for d in ('spv', 'job', 'out'):
        os.makedirs(os.path.join(a.work, d), exist_ok=True)
    G['work'], G['dump'] = a.work, a.dump
    G['fixes'] = set(x for x in a.swarc_fix.split(',') if x)
    G['meta'] = {}
    with open(os.path.join(a.work, 'swarc', 'swarc-meta.jsonl')) as f:
        for line in f:
            j = json.loads(line)
            G['meta'][j['name']] = j
    recs = list(sm3.records(a.shaders))
    if a.limit:
        recs = recs[:a.limit]
    if a.only:
        with open(a.only) as f:
            keep = set(f.read().split())
        recs = [r for r in recs if r[0] in keep]
    write_textures(os.path.join(a.work, 'textures.bin'))
    with multiprocessing.Pool() as pool:
        jobs = pool.map(prepare, recs, chunksize=16)
    errs = [j for j in jobs if 'error' in j]
    with open(os.path.join(a.work, 'jobs.jsonl'), 'w') as f:
        for j in jobs:
            if 'error' not in j:
                f.write(json.dumps(j) + '\n')
    with open(os.path.join(a.work, 'build-errors.txt'), 'w') as f:
        for j in errs:
            f.write('%s\t%s\n' % (j['name'], j['error']))
    print('jobs %d, errors %d' % (len(jobs) - len(errs), len(errs)))
    for j in errs[:10]:
        print('  ', j['name'], j['error'])


if __name__ == '__main__':
    multiprocessing.set_start_method('fork')
    main()
