#!/usr/bin/env python3
"""Statistics for a DXVK pipeline warm file (macOS patch, `pipelines.warm` + `pipelines.warm.shaders`).

usage: warm_stats.py <pipelines.warm> [--shaders <pipelines.warm.shaders>] [--upk <RefShaderCache-PC-D3D-SM3.upk>]
                     [--old-shaders <other .shaders file>]

Prints records per map, distinct (vs, fs) pairs, distinct states, shaders per map and how many maps share a shader.
With --upk it checks that every recorded shader exists byte-for-byte in the game's reference shader cache (a UE3 package
that keeps the D3D9 bytecode uncompressed; shaders are found by scanning for the SM3 version tokens and walking each token
stream to its END token). With --old-shaders it reports how much of an older recording the new one covers.

File formats are the ones written by pc/dxvk/dxvk-3.1.1-macos.patch:
  .warm:    "MEPW", u32 version (2), u32 sizeof(state); records u16 vsLen, u16 fsLen, u16 mapLen, vs, fs, map, state bytes
  .shaders: records u16 nameLen, u8 isPixel, u32 size, name, bytecode
Both hold game-derived data: keep them (and this script's output per shader) out of the repo.
"""
import argparse, collections, re, struct, sys


def read_warm(path):
    d = open(path, "rb").read()
    if d[:4] != b"MEPW":
        sys.exit("not a warm file: " + path)
    _ver, sz = struct.unpack("<II", d[4:12])
    p, out = 12, []
    while p < len(d):
        a, b, m = struct.unpack("<HHH", d[p:p + 6]); p += 6
        vs = d[p:p + a].decode(); p += a
        fs = d[p:p + b].decode(); p += b
        mp = d[p:p + m].decode(); p += m
        out.append((vs, fs, mp, d[p:p + sz])); p += sz
    return out


def read_shaders(path):
    d = open(path, "rb").read()
    p, out = 0, {}
    while p < len(d):
        nl, _px, sz = struct.unpack_from("<HBI", d, p); p += 7
        name = d[p:p + nl].decode(); p += nl
        out[name] = d[p:p + sz]; p += sz
    return out


def walk(buf, p):
    """End offset of the D3D9 shader token stream that starts at p, or None."""
    q = p + 4
    while q + 4 <= len(buf) and q - p < (1 << 20):
        t = struct.unpack_from("<I", buf, q)[0]
        op = t & 0xFFFF
        if t == 0x0000FFFF:
            return q + 4
        if op == 0xFFFE:                      # comment block
            q += 4 + 4 * ((t >> 16) & 0x7FFF)
            continue
        if op > 0x62:                         # not an SM3 opcode
            return None
        q += 4 + 4 * ((t >> 24) & 0xF)
    return None


def upk_shaders(path):
    f = open(path, "rb").read()
    found = set()
    for m in re.finditer(rb"\x00\x03[\xfe\xff]\xff", f):
        e = walk(f, m.start())
        if e:
            found.add(f[m.start():e])
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("warm")
    ap.add_argument("--shaders"); ap.add_argument("--upk"); ap.add_argument("--old-shaders")
    a = ap.parse_args()

    recs = read_warm(a.warm)
    print("records", len(recs), "| distinct (vs,fs) pairs", len({(r[0], r[1]) for r in recs}),
          "| distinct states", len({r[3] for r in recs}))
    print("records per map:", dict(collections.Counter(r[2] for r in recs)))

    bymap = collections.defaultdict(set)
    for vs, fs, m, _ in recs:
        bymap[m] |= {vs, fs}
    print("shaders per map:", {m: len(s) for m, s in sorted(bymap.items())})
    levels = {m: s for m, s in bymap.items() if m not in ("", "tdmainmenu")}
    use = collections.Counter(n for s in levels.values() for n in s)
    print("level shaders: union", len(use), "| used by 1 map", sum(1 for c in use.values() if c == 1),
          "| 2+ maps", sum(1 for c in use.values() if c >= 2), "| 5+ maps", sum(1 for c in use.values() if c >= 5))

    shaders = read_shaders(a.shaders) if a.shaders else None
    if shaders is not None:
        print("recorded shaders", len(shaders))
    if a.upk:
        if shaders is None:
            sys.exit("--upk needs --shaders")
        cache = upk_shaders(a.upk)
        hit = sum(1 for c in shaders.values() if c in cache)
        print("shader cache holds", len(cache), "| recorded found byte-exact:", hit, "of", len(shaders),
              "| cache coverage %.1f%%" % (100 * len({*shaders.values()}) / len(cache)))
    if a.old_shaders:
        if shaders is None:
            sys.exit("--old-shaders needs --shaders")
        old = set(read_shaders(a.old_shaders).values()); new = set(shaders.values())
        print("old recording:", len(old), "| covered by this one:", len(old & new), "| new here:", len(new - old))


if __name__ == "__main__":
    main()
