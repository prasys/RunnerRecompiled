#!/usr/bin/env python3
"""Makes Mach-O files self-contained: copies every non-system dylib they need into one folder and points them at it.

  bundle_dylibs.py exe <lib-dir> <executable> [...]   executables already in place (e.g. vendor/bin/innoextract); they
                                                      get the rpath @executable_path/../lib
  bundle_dylibs.py lib <lib-dir> <library> [...]      libraries to copy into <lib-dir> together with their dependencies

All references become @rpath/<name>; every file changed is re-signed ad hoc (install_name_tool invalidates the
signature and arm64 refuses unsigned code). /usr/lib and /System are left alone. Used by make-dmg.sh; the Homebrew
paths exist only on the build machine.
"""
import os
import re
import shutil
import subprocess
import sys

BREW_LIB_DIRS = ("/opt/homebrew/lib", "/opt/homebrew/opt/llvm@23/lib")


def sh(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout


def own_id(path):
    out = sh("otool", "-D", path).splitlines()
    return out[1].strip() if len(out) > 1 else None


def deps(path):
    me = own_id(path)
    ds = [l.strip().split(" (")[0] for l in sh("otool", "-L", path).splitlines()[1:]]
    return [d for d in ds if d != me]


def rpaths(path):
    out = sh("otool", "-l", path).splitlines()
    r = []
    for i, l in enumerate(out):
        if "cmd LC_RPATH" in l:
            m = re.search(r"path (.+) \(offset", out[i + 2])
            if m:
                r.append(m.group(1))
    return r


def system(d):
    return d.startswith("/usr/lib/") or d.startswith("/System/")


def resolve(dep, orig):
    """The real file behind a dependency reference of `orig` (the file as it was before it was copied)."""
    here = os.path.dirname(orig)
    cands = []
    if dep.startswith("@rpath/"):
        name = dep[len("@rpath/"):]
        for rp in rpaths(orig):
            rp = rp.replace("@loader_path", here).replace("@executable_path", here)
            cands.append(os.path.join(rp, name))
        cands += [os.path.join(here, name)] + [os.path.join(d, name) for d in BREW_LIB_DIRS]
    elif dep.startswith("@loader_path/"):
        cands.append(os.path.join(here, dep[len("@loader_path/"):]))
    else:
        cands.append(dep)
    for c in cands:
        if os.path.exists(c):
            return os.path.realpath(c)
    return None


def sign(p):
    subprocess.run(["codesign", "--force", "-s", "-", p], check=True, capture_output=True)


def fix(target, orig, libdir, done):
    """Copy target's dependencies into libdir (recursively) and rewrite target to reference them by @rpath/<name>."""
    for d in deps(target):
        if system(d):
            continue
        real = resolve(d, orig)
        if real is None:
            print(f"warning: cannot find {d} (needed by {orig})", file=sys.stderr)
            continue
        name = os.path.basename(d)
        dst = os.path.join(libdir, name)
        if name not in done:
            done[name] = real
            shutil.copy2(real, dst)
            os.chmod(dst, 0o755)
            sh("install_name_tool", "-id", "@rpath/" + name, dst)
            if "@loader_path" not in rpaths(dst):
                sh("install_name_tool", "-add_rpath", "@loader_path", dst)
            fix(dst, real, libdir, done)
            sign(dst)
        sh("install_name_tool", "-change", d, "@rpath/" + name, target)
    sign(target)


def main():
    kind, libdir, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(libdir, exist_ok=True)
    done = {}
    for f in files:
        if kind == "exe":
            orig = os.environ.get("BUNDLE_ORIG_" + os.path.basename(f), f)  # where it came from, for @rpath lookups
            fix(f, os.path.realpath(orig), libdir, done)
            if "@executable_path/../lib" not in rpaths(f):
                sh("install_name_tool", "-add_rpath", "@executable_path/../lib", f)
            sign(f)
        else:
            real = os.path.realpath(f)
            name = os.path.basename(f)
            dst = os.path.join(libdir, name)
            shutil.copy2(real, dst)
            os.chmod(dst, 0o755)
            done[name] = real
            sh("install_name_tool", "-id", "@rpath/" + name, dst)
            if "@loader_path" not in rpaths(dst):
                sh("install_name_tool", "-add_rpath", "@loader_path", dst)
            fix(dst, real, libdir, done)
    print(f"bundled {len(done)} libraries into {libdir}")


if __name__ == "__main__":
    main()
