"""Direct3D 9 shader bytecode (SM1-3 tokens): the few things the differential test needs.

Reads the records of a DXVK warm file (`pipelines.warm.shaders`) and lists, per shader, the opcodes used, the
def/defi/defb constants and the dcl'd inputs, outputs and samplers. Original code; token layout per the D3D9 docs
(d3d9types.h, "Shader Code Format").
"""
import struct

OPS = {0: 'nop', 1: 'mov', 2: 'add', 3: 'sub', 4: 'mad', 5: 'mul', 6: 'rcp', 7: 'rsq', 8: 'dp3', 9: 'dp4', 10: 'min',
       11: 'max', 12: 'slt', 13: 'sge', 14: 'exp', 15: 'log', 16: 'lit', 17: 'dst', 18: 'lrp', 19: 'frc', 20: 'm4x4',
       21: 'm4x3', 22: 'm3x4', 23: 'm3x3', 24: 'm3x2', 25: 'call', 26: 'callnz', 27: 'loop', 28: 'ret', 29: 'endloop',
       30: 'label', 31: 'dcl', 32: 'pow', 33: 'crs', 34: 'sgn', 35: 'abs', 36: 'nrm', 37: 'sincos', 38: 'rep',
       39: 'endrep', 40: 'if', 41: 'ifc', 42: 'else', 43: 'endif', 44: 'break', 45: 'breakc', 46: 'mova', 47: 'defb',
       48: 'defi', 64: 'texcoord', 65: 'texkill', 66: 'texld', 80: 'cnd', 81: 'def', 88: 'cmp', 89: 'bem',
       90: 'dp2add', 91: 'dsx', 92: 'dsy', 93: 'texldd', 94: 'setp', 95: 'texldl', 96: 'breakp'}
USAGE = {0: 'position', 1: 'blendweight', 2: 'blendindices', 3: 'normal', 4: 'psize', 5: 'texcoord', 6: 'tangent',
         7: 'binormal', 8: 'tessfactor', 9: 'positiont', 10: 'color', 11: 'fog', 12: 'depth', 13: 'sample'}
SRCMOD = {0: '', 1: 'neg', 2: 'bias', 3: 'biasneg', 4: 'sign', 5: 'signneg', 6: 'comp', 7: 'x2', 8: 'x2neg', 9: 'dz',
          10: 'dw', 11: 'abs', 12: 'absneg', 13: 'not'}
NO_DST = {0, 25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96}


def records(path):
    """Yields (name, is_pixel, bytecode) from a pipelines.warm.shaders file."""
    with open(path, 'rb') as f:
        data = f.read()
    p = 0
    while p + 7 <= len(data):
        nl, px, sz = struct.unpack_from('<HBI', data, p)
        p += 7
        name = data[p:p + nl].decode()
        p += nl
        yield name, bool(px), data[p:p + sz]
        p += sz


def regtype(tok):
    return ((tok >> 28) & 7) | ((tok >> 8) & 0x18)


class Shader:
    def __init__(self, code):
        t = struct.unpack('<%dI' % (len(code) // 4), code[:len(code) // 4 * 4])
        self.pixel = (t[0] >> 16) == 0xFFFF
        self.major, self.minor = (t[0] >> 8) & 0xFF, t[0] & 0xFF
        self.ops = []        # opcode names in order (texld variants named, comparisons kept apart)
        self.features = set()
        self.defs, self.defi, self.defb = {}, {}, {}
        self.inputs, self.outputs, self.samplers = {}, [], {}   # reg -> (usage, index, mask); list of (reg, usage, index, mask)
        self.misc = set()
        self.consts_read = set()
        i = 1
        while i < len(t):
            tok = t[i]
            op = tok & 0xFFFF
            if op == 0xFFFF:
                break
            if op == 0xFFFE:
                i += 1 + ((tok >> 16) & 0x7FFF)
                continue
            n = (tok >> 24) & 0xF
            a = t[i + 1:i + 1 + n]
            ctl = (tok >> 16) & 0xFF
            name = OPS.get(op, 'op%d' % op)
            if op == 81:
                self.defs[a[0] & 0x7FF] = struct.unpack('<4f', struct.pack('<4I', *a[1:5]))
            elif op == 48:
                self.defi[a[0] & 0x7FF] = struct.unpack('<4i', struct.pack('<4I', *a[1:5]))
            elif op == 47:
                self.defb[a[0] & 0x7FF] = bool(a[1])
            elif op == 31:
                u, d = a[0], a[1]
                rt, num, mask = regtype(d), d & 0x7FF, (d >> 16) & 0xF
                if rt == 10:
                    self.samplers[num] = {2: '2d', 3: 'cube', 4: '3d'}.get((u >> 27) & 0xF, '2d')
                elif rt == 17:
                    self.misc.add('vpos' if num == 0 else 'vface')
                    self.features.add('vpos' if num == 0 else 'vface')
                elif rt == 1:
                    self.inputs[num] = (USAGE.get(u & 0x1F, '?'), (u >> 16) & 0xF, mask)
                elif rt == 6:
                    self.outputs.append((num, USAGE.get(u & 0x1F, '?'), (u >> 16) & 0xF, mask))
            else:
                if op == 66:
                    name = ('texld', 'texldp', 'texldb')[ctl] if ctl < 3 else 'texld?'
                self.ops.append(name)
                self.features.add(name)
                k = 0
                if op not in NO_DST and n:
                    d = a[0]
                    k = 1
                    if d & 0x2000:
                        k += 1
                    if (d >> 20) & 1:
                        self.features.add('sat')
                    if (d >> 20) & 2:
                        self.features.add('pp')
                    if (d >> 20) & 4:
                        self.features.add('centroid')
                    if regtype(d) == 9:
                        self.features.add('odepth')
                    if regtype(d) == 8 and (d & 0x7FF):
                        self.features.add('mrt')
                    if tok & 0x10000000:
                        self.features.add('predicated')
                        k += 1
                while k < n:
                    s = a[k]
                    k += 1
                    m = (s >> 24) & 0xF
                    if m:
                        self.features.add('src:' + SRCMOD.get(m, str(m)))
                    rt = regtype(s)
                    if rt == 2:
                        self.consts_read.add(s & 0x7FF)
                    if rt == 7:
                        self.features.add('consti')
                    if rt == 14:
                        self.features.add('constb')
                    if s & 0x2000:
                        self.features.add('reladdr')
                        k += 1
            i += 1 + n


if __name__ == '__main__':
    import sys
    import collections
    cnt = {False: collections.Counter(), True: collections.Counter()}
    tot = collections.Counter()
    ver = collections.Counter()
    for name, px, code in records(sys.argv[1]):
        s = Shader(code)
        tot[px] += 1
        ver[('ps' if s.pixel else 'vs') + '_%d_%d' % (s.major, s.minor)] += 1
        for f in s.features | {'smp:' + v for v in s.samplers.values()}:
            cnt[px][f] += 1
    print('versions', dict(ver))
    for px in (False, True):
        print('pixel' if px else 'vertex', tot[px])
        print('  ' + ', '.join('%s %d' % kv for kv in sorted(cnt[px].items(), key=lambda kv: -kv[1])))
