#!/usr/bin/env python3
"""Warm-up manifest: a pipeline recording without the shader bytecode, for LOCAL USE ONLY.

A manifest (shader hashes + pipeline states + map names) is derived from game data. It must never be committed to
the repo or distributed to anyone (DECISIONS.md). This tool is not a distribution mechanism: it is for the user's
own recording, e.g. a compact backup, or moving a recording between the user's own machines that each have the
user's own copy of the game.

usage: warm_manifest.py export <pipelines.warm> <pipelines.warm.shaders> -o <manifest> [--upk <RefShaderCache upk>]
       warm_manifest.py import <manifest> <RefShaderCache-PC-D3D-SM3.upk> -o <dir> [--force] [--strict]
       warm_manifest.py info   <manifest>

export turns a recording made by the patched DXVK (pc/dxvk/dxvk-3.1.1-macos.patch, "Pipeline warm-up") into a manifest
that names every shader by a hash of its bytecode instead of carrying the bytecode. import rebuilds `pipelines.warm`
and `pipelines.warm.shaders` on another of the user's machines, taking the bytecode from the user's own copy of the game's
reference shader cache (a UE3 package with the D3D9 bytecode uncompressed; shaders are found by scanning for the SM3
version tokens and walking each token stream to its END token, as warm_stats.py does).

Shader names. The warm files refer to shaders by DXVK's name, `vs.<32 hex>` / `fs.<32 hex>`. For D3D9 that is
    md5(bytecode + bytes of D3D9ShaderOptions).hexdigest()
(d3d9_device.cpp hashes the bytecode and then `m_shaderOptions`; DxvkShaderHash folds the 16 digest bytes into four
dwords and prints them byte by byte, which is the plain hex digest). D3D9ShaderOptions is 4 bytes in the patched tree:
isSWVP, d3d9FloatEmulation (0 off, 1 on, 2 strict), forceSamplerTypeSpecConstants, emulateDepthCompare. export finds
those 4 bytes by search and checks them against every recorded (name, bytecode) pair; they go into the manifest
header and import uses them. They depend on the DXVK configuration and the Vulkan driver (float emulation is "strict"
on a driver with the mulz feature, emulateDepthCompare is on for MoltenVK), so a manifest fits the configuration it was
recorded with. A wrong value is harmless but useless: the names match nothing and DXVK ignores the records.

Manifest layout (little endian; the whole file may be gzip-compressed, import detects that):
    "MEWM", u32 format version (1), u32 header length, header (JSON, UTF-8)
    shader table   counts.shaders x hash_bytes     sha256(bytecode) cut to hash_bytes (default 16 = 128 bits)
    state table    counts.states  x state_size     raw DxvkGraphicsPipelineStateInfo, each distinct state once
    map table      counts.maps    x (u16 length, name)
    records        counts.records x (u32 vs, u32 fs, u32 state, u16 map)   indices; fs 0xFFFFFFFF = no pixel shader
Records keep the order of the recording, so import reproduces the original two files byte for byte when every shader is
found. The state bytes are DXVK's in-memory struct: they belong to one DXVK build (header: dxvk, state_size).

The manifest holds no bytecode but is still derived data; the files import writes hold bytecode. Keep all of them out
of the repo (under work/ or a scratch directory) and do not distribute them.
"""
import argparse, gzip, hashlib, itertools, json, mmap, os, re, struct, sys, time

MAGIC = b"MEWM"
FORMAT = 1
WARM_MAGIC = b"MEPW"
WARM_VERSION = 2
NO_SHADER = 0xFFFFFFFF
REC = struct.Struct("<IIIH")
DXVK_DEFAULT = "3.1.1+dxvk-3.1.1-macos.patch"


def warn(msg):
    print("warning: " + msg, file=sys.stderr)


# ---- warm files (formats written by the DXVK patch) ----

def read_warm(path):
    """-> (version, state size, [(vs name, fs name, map, state bytes)])"""
    d = open(path, "rb").read()
    if d[:4] != WARM_MAGIC or len(d) < 12:
        sys.exit("not a warm file: " + path)
    ver, sz = struct.unpack_from("<II", d, 4)
    if ver != WARM_VERSION:
        sys.exit("%s: warm file version %d, this tool knows %d" % (path, ver, WARM_VERSION))
    p, out = 12, []
    while p + 6 <= len(d):
        a, b, m = struct.unpack_from("<HHH", d, p)
        q = p + 6 + a + b + m + sz
        if q > len(d):
            break                                  # torn last record (the game was killed while writing)
        p += 6
        vs = d[p:p + a].decode("ascii"); p += a
        fs = d[p:p + b].decode("ascii"); p += b
        mp = d[p:p + m].decode("utf-8"); p += m
        out.append((vs, fs, mp, d[p:p + sz])); p = q
    if p != len(d):
        warn("%s: %d trailing bytes ignored (incomplete record)" % (path, len(d) - p))
    return ver, sz, out


def read_shaders(path):
    """-> {name: bytecode}, in file order"""
    d = open(path, "rb").read()
    p, out = 0, {}
    while p + 7 <= len(d):
        nl, _px, sz = struct.unpack_from("<HBI", d, p)
        if p + 7 + nl + sz > len(d):
            break
        p += 7
        name = d[p:p + nl].decode("ascii"); p += nl
        out[name] = d[p:p + sz]; p += sz
    if p != len(d):
        warn("%s: %d trailing bytes ignored (incomplete record)" % (path, len(d) - p))
    return out


def write_warm(path, state_size, recs):
    with open(path, "wb") as f:
        f.write(WARM_MAGIC + struct.pack("<II", WARM_VERSION, state_size))
        for vs, fs, mp, state in recs:
            v, s, m = vs.encode("ascii"), fs.encode("ascii"), mp.encode("utf-8")
            f.write(struct.pack("<HHH", len(v), len(s), len(m)) + v + s + m + state)


def write_shaders(path, shaders):
    with open(path, "wb") as f:
        for name, code in shaders:
            n = name.encode("ascii")
            f.write(struct.pack("<HBI", len(n), 1 if is_pixel(code) else 0, len(code)) + n + code)


# ---- shaders ----

def is_pixel(code):
    return code[2:4] == b"\xff\xff"                # version token 0xFFFF03xx pixel, 0xFFFE03xx vertex


def shader_name(code, options):
    return ("fs." if is_pixel(code) else "vs.") + hashlib.md5(code + options).hexdigest()


def content_hash(code, nbytes):
    return hashlib.sha256(code).digest()[:nbytes]


def walk(buf, p):
    """End offset of the D3D9 shader token stream that starts at p, or None."""
    q, n = p + 4, len(buf)
    unpack = struct.unpack_from
    while q + 4 <= n and q - p < (1 << 20):
        t = unpack("<I", buf, q)[0]
        op = t & 0xFFFF
        if t == 0x0000FFFF:
            return q + 4
        if op == 0xFFFE:                           # comment block
            q += 4 + 4 * ((t >> 16) & 0x7FFF)
            continue
        if op > 0x62:                              # not an SM3 opcode
            return None
        q += 4 + 4 * ((t >> 24) & 0xF)
    return None


def upk_shaders(buf):
    """Yield the bytecode of every SM3 shader found in a package."""
    for m in re.finditer(rb"\x00\x03[\xfe\xff]\xff", buf):
        e = walk(buf, m.start())
        if e:
            yield buf[m.start():e]


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 22), b""):
            h.update(blk)
    return h.hexdigest()


def find_options(shaders):
    """The 4 D3D9ShaderOptions bytes that reproduce every recorded name, or None."""
    items = list(shaders.items())
    name0, code0 = items[0]
    for cand in itertools.product(range(4), repeat=4):        # three bools and an enum 0..2
        opts = bytes(cand)
        if shader_name(code0, opts) == name0 and all(shader_name(c, opts) == n for n, c in items):
            return opts
    return None


# ---- manifest ----

def load_manifest(path):
    d = open(path, "rb").read()
    if d[:2] == b"\x1f\x8b":
        d = gzip.decompress(d)
    if d[:4] != MAGIC or len(d) < 12:
        sys.exit("not a warm manifest: " + path)
    fmt, hlen = struct.unpack_from("<II", d, 4)
    if fmt != FORMAT:
        sys.exit("%s: manifest format %d, this tool knows %d" % (path, fmt, FORMAT))
    if 12 + hlen > len(d):
        sys.exit(path + ": truncated header")
    try:
        hdr = json.loads(d[12:12 + hlen].decode("utf-8"))
        c = hdr["counts"]
        ns, nst, nm, nr = int(c["shaders"]), int(c["states"]), int(c["maps"]), int(c["records"])
        hb, ssz = int(hdr["hash_bytes"]), int(hdr["state_size"])
        opts = bytes.fromhex(hdr["shader_options"])
    except (ValueError, KeyError, TypeError) as e:
        sys.exit("%s: bad header (%s)" % (path, e))
    if min(ns, nst, nm, nr) < 0 or not 4 <= hb <= 32 or not 0 < ssz <= 65536:
        sys.exit(path + ": bad header values")
    p = 12 + hlen
    if p + ns * hb + nst * ssz > len(d):
        sys.exit(path + ": truncated tables")
    hashes = [d[p + i * hb:p + (i + 1) * hb] for i in range(ns)]; p += ns * hb
    states = [d[p + i * ssz:p + (i + 1) * ssz] for i in range(nst)]; p += nst * ssz
    maps = []
    for _ in range(nm):
        if p + 2 > len(d):
            sys.exit(path + ": truncated map table")
        n = struct.unpack_from("<H", d, p)[0]
        maps.append(d[p + 2:p + 2 + n].decode("utf-8")); p += 2 + n
    if p + nr * REC.size != len(d):
        sys.exit(path + ": record table has the wrong size")
    recs = [REC.unpack_from(d, p + i * REC.size) for i in range(nr)]
    for v, f, s, m in recs:
        if v >= ns or (f >= ns and f != NO_SHADER) or s >= nst or m >= nm:
            sys.exit(path + ": record index out of range")
    return hdr, opts, hashes, states, maps, recs, len(d)


def cmd_export(a):
    _ver, ssz, recs = read_warm(a.warm)
    shaders = read_shaders(a.shaders)
    if not recs or not shaders:
        sys.exit("empty recording")
    if not 4 <= a.hash_bytes <= 32:
        sys.exit("--hash-bytes must be 4..32")

    opts = find_options(shaders)
    if opts is None:
        sys.exit("cannot reproduce the recorded shader names from the bytecode (md5 of bytecode + 4 option bytes): "
                 "this recording was not made by the DXVK build this tool knows")

    hash_of = {n: content_hash(c, a.hash_bytes) for n, c in shaders.items()}
    if len(set(hash_of.values())) != len({bytes(c) for c in shaders.values()}):
        sys.exit("two different shaders share a %d-byte hash: raise --hash-bytes" % a.hash_bytes)

    sh_idx, st_idx, mp_idx, out, dropped = {}, {}, {}, [], 0
    for vs, fs, mp, state in recs:
        if vs not in hash_of or (fs and fs not in hash_of):
            dropped += 1                           # a record whose bytecode was never saved cannot be rebuilt
            continue
        v = sh_idx.setdefault(hash_of[vs], len(sh_idx))
        f = sh_idx.setdefault(hash_of[fs], len(sh_idx)) if fs else NO_SHADER
        out.append(REC.pack(v, f, st_idx.setdefault(state, len(st_idx)), mp_idx.setdefault(mp, len(mp_idx))))
    if dropped:
        warn("%d records name a shader that is not in the .shaders file: left out" % dropped)
    if len(mp_idx) > 0xFFFF:
        sys.exit("too many maps")

    hdr = {
        "format": "mirrors-edge warm manifest",
        "dxvk": a.dxvk,
        "warm_version": WARM_VERSION,
        "state_size": ssz,
        "shader_options": opts.hex(),
        "hash": "sha256",
        "hash_bytes": a.hash_bytes,
        "counts": {"shaders": len(sh_idx), "states": len(st_idx), "maps": len(mp_idx), "records": len(out)},
    }
    if a.upk:
        with open(a.upk, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as buf:
            have = {content_hash(c, a.hash_bytes) for c in upk_shaders(buf)}
        missing = sum(1 for h in sh_idx if h not in have)
        if missing:
            warn("%d of %d recorded shaders are not in %s" % (missing, len(sh_idx), a.upk))
        hdr["upk"] = {"sha256": file_sha256(a.upk), "size": os.path.getsize(a.upk)}

    hj = json.dumps(hdr, sort_keys=True, separators=(",", ":")).encode("utf-8")
    body = b"".join([MAGIC, struct.pack("<II", FORMAT, len(hj)), hj, *sh_idx, *st_idx,
                     *(struct.pack("<H", len(m.encode("utf-8"))) + m.encode("utf-8") for m in mp_idx), *out])
    data = gzip.compress(body, 9, mtime=0) if a.output.endswith(".gz") else body
    with open(a.output, "wb") as f:
        f.write(data)
    print("manifest: %d records, %d shaders, %d states, %d maps | shader options %s | %d bytes%s"
          % (len(out), len(sh_idx), len(st_idx), len(mp_idx), opts.hex(), len(body),
             " (%d gzip)" % len(data) if data is not body else ""))


def cmd_import(a):
    t0 = time.perf_counter()
    hdr, opts, hashes, states, maps, recs, _raw = load_manifest(a.manifest)
    if a.shader_options:
        opts = bytes.fromhex(a.shader_options)
    if hdr.get("warm_version") != WARM_VERSION:
        sys.exit("manifest is for warm file version %r, this tool writes %d" % (hdr.get("warm_version"), WARM_VERSION))
    hb = len(hashes[0]) if hashes else 16

    warm_path = os.path.join(a.output, "pipelines.warm")
    for p in (warm_path, warm_path + ".shaders"):
        if os.path.exists(p) and not a.force:
            sys.exit(p + " exists (use --force to replace it)")

    warnings = 0
    want = set(hashes)
    found, clash = {}, set()
    with open(a.upk, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        scanned = 0
        for code in upk_shaders(buf):
            scanned += 1
            h = content_hash(code, hb)
            if h in want and found.setdefault(h, code) != code:
                clash.add(h)                       # two different shaders, one short hash: do not guess
        up = hdr.get("upk")
        if up and not a.no_upk_hash:
            if os.path.getsize(a.upk) != up.get("size") or file_sha256(a.upk) != up.get("sha256"):
                warn("the shader cache differs from the one the manifest was made with (another game build?)")
                warnings += 1
    for h in clash:
        del found[h]
    if clash:
        warn("%d manifest hashes match more than one shader in the cache: skipped" % len(clash)); warnings += 1

    names = [shader_name(found[h], opts) if h in found else None for h in hashes]
    missing = sum(1 for n in names if n is None)
    out, used, order, skipped, wrong_stage = [], set(), [], 0, 0
    for v, f, s, m in recs:
        if names[v] is None or (f != NO_SHADER and names[f] is None):
            skipped += 1
            continue
        if is_pixel(found[hashes[v]]) or (f != NO_SHADER and not is_pixel(found[hashes[f]])):
            wrong_stage += 1
            continue
        for i in ((v,) if f == NO_SHADER else (v, f)):
            if i not in used:
                used.add(i); order.append((names[i], found[hashes[i]]))
        out.append((names[v], "" if f == NO_SHADER else names[f], maps[m], states[s]))
    if missing:
        warn("%d of %d shaders are not in the shader cache: %d records skipped" % (missing, len(hashes), skipped))
        warnings += 1
    if wrong_stage:
        warn("%d records skipped: vertex/pixel stage does not fit" % wrong_stage); warnings += 1

    os.makedirs(a.output, exist_ok=True)
    write_warm(warm_path, int(hdr["state_size"]), out)
    write_shaders(warm_path + ".shaders", order)
    print("import: %d of %d records, %d of %d shaders (%d token streams scanned in the cache) -> %s | %.2f s"
          % (len(out), len(recs), len(order), len(hashes), scanned, warm_path, time.perf_counter() - t0))
    print("for DXVK %s, state size %d (DXVK discards the file if its own state size differs)"
          % (hdr.get("dxvk"), int(hdr["state_size"])))
    if a.strict and warnings:
        sys.exit(1)


def cmd_info(a):
    hdr, _opts, _hashes, _states, maps, recs, raw = load_manifest(a.manifest)
    print(json.dumps(hdr, indent=1, sort_keys=True))
    per = {}
    for _v, _f, _s, m in recs:
        per[maps[m]] = per.get(maps[m], 0) + 1
    print("records per map:", per)
    print("size: %d bytes raw, %d on disk" % (raw, os.path.getsize(a.manifest)))


def main():
    ap = argparse.ArgumentParser(description="Export / import a pipeline warm-up manifest (no shader bytecode). Local use only: "
                                             "a manifest is derived from game data, never commit or distribute it.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("export", help="recording -> manifest")
    e.add_argument("warm"); e.add_argument("shaders")
    e.add_argument("-o", "--output", required=True, help="manifest path; a name ending in .gz is gzip-compressed")
    e.add_argument("--upk", help="the shader cache the recording came from: its sha256 goes into the header")
    e.add_argument("--dxvk", default=DXVK_DEFAULT, help="DXVK build the states belong to (default %(default)s)")
    e.add_argument("--hash-bytes", type=int, default=16, help="bytes of sha256 kept per shader (default 16)")
    e.set_defaults(fn=cmd_export)

    i = sub.add_parser("import", help="manifest + the game's shader cache -> pipelines.warm, pipelines.warm.shaders")
    i.add_argument("manifest"); i.add_argument("upk")
    i.add_argument("-o", "--output", required=True, help="directory for the two files")
    i.add_argument("--force", action="store_true", help="replace existing files")
    i.add_argument("--strict", action="store_true", help="exit 1 if anything was skipped or the cache differs")
    i.add_argument("--no-upk-hash", action="store_true", help="do not compare the cache's sha256 with the header")
    i.add_argument("--shader-options", help="override the header's D3D9ShaderOptions bytes (8 hex digits)")
    i.set_defaults(fn=cmd_import)

    n = sub.add_parser("info", help="print a manifest's header and counts")
    n.add_argument("manifest")
    n.set_defaults(fn=cmd_info)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
