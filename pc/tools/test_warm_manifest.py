#!/usr/bin/env python3
"""Round-trip test for warm_manifest.py. This file contains no data. The recording, the shader cache and any
manifest are game-derived: they are never in the repo and never distributed, so pass the paths in:

usage: python3 -I pc/tools/test_warm_manifest.py <pipelines.warm> <pipelines.warm.shaders> <RefShaderCache upk> [--tmp <dir>]

1. export the recording to a manifest (raw and .gz); check the manifest carries no shader bytecode
2. import it against the shader cache; check every shader's bytes and every record (vs bytecode, fs bytecode, state,
   map) against the original, and whether the two files came back byte for byte
3. negative case: the same import against a copy of the cache in which one recorded pixel shader is damaged
   (stands for another game build): the records that use it must be skipped with a warning, the rest must match

Prints the manifest sizes and the import time. Everything is written to a temporary directory and deleted.
The readers here are separate from the tool's on purpose.
"""
import argparse, gzip, os, shutil, struct, subprocess, sys, tempfile, time, zlib

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "warm_manifest.py")


def read_warm(path):
    d = open(path, "rb").read()
    assert d[:4] == b"MEPW", path
    ver, sz = struct.unpack_from("<II", d, 4)
    p, out = 12, []
    while p < len(d):
        a, b, m = struct.unpack_from("<HHH", d, p); p += 6
        vs = d[p:p + a]; p += a
        fs = d[p:p + b]; p += b
        mp = d[p:p + m]; p += m
        out.append((vs, fs, mp, d[p:p + sz])); p += sz
    assert p == len(d), "torn warm file " + path
    return ver, sz, out


def read_shaders(path):
    d = open(path, "rb").read()
    p, out = 0, {}
    while p < len(d):
        nl, px, sz = struct.unpack_from("<HBI", d, p); p += 7
        name = d[p:p + nl]; p += nl
        assert name not in out, "shader stored twice"
        out[name] = (px, d[p:p + sz]); p += sz
    assert p == len(d), "torn shaders file " + path
    return out


def resolve(recs, shaders):
    """Records with the names replaced by what they stand for: (vs bytecode, fs bytecode, state, map)."""
    return [(shaders[vs][1], shaders[fs][1] if fs else b"", st, mp) for vs, fs, mp, st in recs]


def tool(*args):
    r = subprocess.run([sys.executable, "-I", TOOL, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return r.returncode, r.stdout, r.stderr


def check(cond, what):
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        check.failed += 1
check.failed = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("warm"); ap.add_argument("shaders"); ap.add_argument("upk"); ap.add_argument("--tmp")
    a = ap.parse_args()

    _ver, ssz, recs0 = read_warm(a.warm)
    sh0 = read_shaders(a.shaders)
    orig = resolve(recs0, sh0)
    print("recording: %d records, %d shaders, state size %d" % (len(recs0), len(sh0), ssz))

    tmp = tempfile.mkdtemp(prefix="warm-manifest-test-", dir=a.tmp)
    try:
        # 1. export
        man, mangz = os.path.join(tmp, "warm.manifest"), os.path.join(tmp, "warm.manifest.gz")
        rc, out, err = tool("export", a.warm, a.shaders, "-o", man, "--upk", a.upk)
        print(out.strip()); sys.stderr.write(err)
        check(rc == 0 and not err, "export (with --upk) exits 0 without warnings")
        rc, _, _ = tool("export", a.warm, a.shaders, "-o", mangz, "--upk", a.upk)
        raw = open(man, "rb").read()
        check(rc == 0 and gzip.decompress(open(mangz, "rb").read()) == raw, ".gz export holds the same manifest")
        long_enough = [c for _, c in sh0.values() if len(c) >= 32]
        check(not any(c[:32] in raw for c in long_enough) and not any(c[-32:] in raw for c in long_enough),
              "manifest contains no shader bytecode (first and last 32 bytes of %d shaders searched)" % len(long_enough))
        print("manifest size: %d bytes raw, %d gzip -9, %d zlib -9"
              % (len(raw), os.path.getsize(mangz), len(zlib.compress(raw, 9))))
        print("recording size for comparison: %d + %d bytes"
              % (os.path.getsize(a.warm), os.path.getsize(a.shaders)))

        # 2. import, positive
        outdir = os.path.join(tmp, "out")
        t = time.perf_counter()
        rc, out, err = tool("import", mangz, a.upk, "-o", outdir, "--strict")
        dt = time.perf_counter() - t
        print(out.strip()); sys.stderr.write(err)
        check(rc == 0 and not err, "import --strict exits 0 without warnings")
        print("import time: %.2f s wall (whole process, gzip manifest)" % dt)
        w1, s1 = os.path.join(outdir, "pipelines.warm"), os.path.join(outdir, "pipelines.warm.shaders")
        _v, ssz1, recs1 = read_warm(w1)
        sh1 = read_shaders(s1)
        check(ssz1 == ssz, "state size in the header")
        check(sh1 == sh0, "every shader: name, stage flag and bytes (%d)" % len(sh1))
        check(resolve(recs1, sh1) == orig, "every record: vs bytecode, fs bytecode, state, map, in order (%d)" % len(recs1))
        check(recs1 == recs0, "every record's shader names")
        check(open(w1, "rb").read() == open(a.warm, "rb").read(), "pipelines.warm is byte-identical to the recording")
        check(open(s1, "rb").read() == open(a.shaders, "rb").read(), "pipelines.warm.shaders is byte-identical to the recording")
        rc, _, err = tool("import", man, a.upk, "-o", outdir)
        check(rc != 0 and "exists" in err, "import refuses to overwrite without --force")

        # 3. negative: one recorded pixel shader is not in the cache
        victim = next(n for n, (px, c) in sh0.items() if px == 1 and len(c) >= 64)
        code = sh0[victim][1]
        upk2 = os.path.join(tmp, "other-build.upk")
        shutil.copyfile(a.upk, upk2)
        with open(upk2, "r+b") as f:
            data = f.read()
            pos, n = data.find(code), 0
            while pos >= 0:                       # damage every copy: a byte in the last instruction's operand
                f.seek(pos + len(code) - 6); f.write(bytes([code[-6] ^ 0x01])); n += 1
                pos = data.find(code, pos + 1)
            del data
        check(n >= 1, "victim shader located in the cache copy (%d place(s))" % n)
        expect = [r for r, (vs, fs, _m, _s) in zip(orig, recs0) if victim not in (vs, fs)]
        outdir2 = os.path.join(tmp, "out2")
        rc, out, err = tool("import", man, upk2, "-o", outdir2)
        print(out.strip()); print("  stderr: " + err.strip().replace("\n", "\n  stderr: "))
        check(rc == 0, "import with a missing shader exits 0")
        check("warning" in err and "not in the shader cache" in err, "it warns about the missing shader")
        check("differs" in err, "it warns that the cache is not the one the manifest was made with")
        _v, _s, recs2 = read_warm(os.path.join(outdir2, "pipelines.warm"))
        sh2 = read_shaders(os.path.join(outdir2, "pipelines.warm.shaders"))
        check(victim not in sh2 and len(recs2) == len(expect) < len(recs0),
              "the %d record(s) using it are skipped, %d kept" % (len(recs0) - len(expect), len(recs2)))
        check(resolve(recs2, sh2) == expect, "the kept records match the original")
        check(all(sh2[n] == sh0[n] for n in sh2), "the kept shaders match the original (%d)" % len(sh2))
        rc, _, _ = tool("import", man, upk2, "-o", outdir2, "--force", "--strict")
        check(rc == 1, "--strict turns the skip into exit code 1")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("FAILED: %d check(s)" % check.failed if check.failed else "all checks passed")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
