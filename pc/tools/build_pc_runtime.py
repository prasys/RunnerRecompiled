"""Builds the lifted PC game for a Windows x64 host (bring-up): generated shards of the game and
of the CRT DLLs, the runtime, one executable.

From the project root:
  python -I outputs/mirrors-edge-native/pc/tools/build_pc_runtime.py \
      --toolchain work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64 --work work/pc-recomp [--regen]

macOS (arm64): same, with  --macos /opt/homebrew/opt/llvm@23  instead of --toolchain.

Expects catalogs in work/pc-recomp (game: catalog.json; msvcr80/, msvcp80/), the game in
work/pc/Binaries and the CRT DLLs in work/pc-deps. Output: work/pc-recomp/bin/me_pc.exe.
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.normpath(os.path.join(HERE, "..", "runtime"))
CFLAGS = ["-O2", "-g0", "-fwrapv", "-ffp-contract=off", "-fno-strict-aliasing", "-Wno-unused-label",
          "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function"]

# tag, binary to lift, catalog, guest module name, folder at run time (game | deps | rebased),
# DllMain at start-up (1) or on the first LoadLibrary (0). The PhysX DLLs all ask for 0x10000000:
# they are lifted from copies rebased with rebase_pe.py (0x20000000 + n * 0x01000000; the
# audio DLLs from 0x26000000).
MODULES = [
    ("msvcr80", "work/pc-deps/msvcr80.dll", "msvcr80/catalog.json", "msvcr80.dll", "deps", 1),
    ("msvcp80", "work/pc-deps/msvcp80.dll", "msvcp80/catalog.json", "msvcp80.dll", "deps", 1),
    ("physxloader", "work/pc-deps/rebased/PhysXLoader.dll", "physxloader/catalog.json", "physxloader.dll", "rebased", 0),
    ("physxcore", "work/pc-deps/rebased/PhysXCore.dll", "physxcore/catalog.json", "physxcore.dll", "rebased", 0),
    ("physxcooking", "work/pc-deps/rebased/PhysXCooking.dll", "physxcooking/catalog.json", "physxcooking.dll", "rebased", 0),
    ("nxcooking", "work/pc-deps/rebased/NxCooking.dll", "nxcooking/catalog.json", "nxcooking.dll", "rebased", 0),
    ("physxextensions", "work/pc-deps/rebased/PhysXExtensions.dll", "physxextensions/catalog.json", "physxextensions.dll", "rebased", 0),
    ("nxcharacter", "work/pc-deps/rebased/NxCharacter.dll", "nxcharacter/catalog.json", "nxcharacter.dll", "rebased", 0),
    # Ogg Vorbis decoding and resampling of the game's audio (static imports of the game).
    ("ogg", "work/pc-deps/rebased/ogg.dll", "ogg/catalog.json", "ogg.dll", "rebased", 0),
    ("vorbis", "work/pc-deps/rebased/vorbis.dll", "vorbis/catalog.json", "vorbis.dll", "rebased", 0),
    ("vorbisfile", "work/pc-deps/rebased/vorbisfile.dll", "vorbisfile/catalog.json", "vorbisfile.dll", "rebased", 0),
    ("vorbisenc", "work/pc-deps/rebased/vorbisenc.dll", "vorbisenc/catalog.json", "vorbisenc.dll", "rebased", 0),
    ("libresample", "work/pc-deps/rebased/libresample.dll", "libresample/catalog.json", "libresample.dll", "rebased", 0),
    # Bink video (static import of the game; several code sections, see pe32.code_blob).
    ("binkw32", "work/pc-deps/rebased/binkw32.dll", "binkw32/catalog.json", "binkw32.dll", "rebased", 0),
    ("game", "work/pc/Binaries/MirrorsEdge.exe", "catalog.json", "mirrorsedge.exe", "game", 0),
]


def write_module_table(path):
    """rt_modules_gen.c: the modules rt_main loads, in load order (the game last)."""
    lines = ['#include "rt.h"', ""]
    for tag, *_ in MODULES:
        lines.append(f"extern const X86FnEntry x86_fns_{tag}[];")
        lines.append(f"extern const uint32_t x86_fns_{tag}_count;")
    lines.append("")
    lines.append("const RtModuleSpec rt_module_specs[] = {")
    for tag, binary, _, name, kind, init in MODULES:
        lines.append(f'    {{"{name}", "{kind}", "{os.path.basename(binary)}", x86_fns_{tag}, &x86_fns_{tag}_count, {init}}},')
    lines.append("};")
    lines.append(f"const int rt_module_spec_count = {len(MODULES)};")
    new = "\n".join(lines) + "\n"
    if not os.path.exists(path) or open(path).read() != new:
        open(path, "w").write(new)


SHATTER = os.path.normpath(os.path.join(HERE, "..", "shatter"))
# Shatter (github.com/Arecsu/shatter, MIT): PhysX fluids on a native solver (pc/shatter/UPSTREAM.md). macOS arm64 only
# for now; SSE intrinsics go through sse2neon (pc/shatter/port/neon).
SHATTER_SOURCES = ("solver/fluid/fluid.cpp", "solver/cloth/cloth.cpp", "solver/cloth/cloth_attach.cpp",
                   "solver/cloth/cloth_edgeshape.cpp", "solver/cloth/cloth_mesh.cpp", "solver/cloth/cloth_self.cpp",
                   "solver/cloth/cloth_selftri.cpp", "solver/cloth/cloth_state.cpp", "shim/fluid_ours.cpp",
                   "shim/cloth_ours.cpp", "shim/scene_read.cpp", "shim/coverage.cpp", "port/bridge.cpp",
                   "port/game_hooks_port.cpp")


def compile_shatter(cxx, src, obj, extra=()):
    # any file of pc/shatter (or the runtime headers guest.h includes) makes every Shatter object stale
    deps = [os.path.join(d, f) for d, _, fs in os.walk(SHATTER) for f in fs] + \
           [os.path.join(RUNTIME, h) for h in ("rt.h", "x86rt.h")]
    if os.path.exists(obj) and all(os.path.getmtime(obj) >= os.path.getmtime(d) for d in deps):
        return obj, None
    inc = []
    for d in ("port", "port/neon", "shim"):
        inc += ["-I", os.path.join(SHATTER, d)]
    r = subprocess.run([cxx, *CFLAGS, *extra, "-std=c++17", "-I", RUNTIME, *inc, "-c", src, "-o", obj],
                       capture_output=True, text=True)
    return obj, (r.stderr if r.returncode else None)


def compile_one(cc, src, obj, extra=()):
    # headers every file depends on (runtime files also on rt.h and the D3D9 bridge headers)
    headers = ["x86rt.h"] + (["rt.h", "rt_d3d9.h", "rt_d3d9_idx.h"]
                             if os.path.dirname(os.path.abspath(src)) == RUNTIME else [])
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and \
            all(os.path.getmtime(obj) >= os.path.getmtime(os.path.join(RUNTIME, h)) for h in headers):
        return obj, None
    flags = list(extra)
    if os.path.basename(src) == "rt_main.c" and ("-fPIC" in flags or "-arch" in flags):
        flags.append("-Dmain=me_pc_main")  # Android: a library entry the app calls; macOS: rt_host_macos.c has main()
    r = subprocess.run([cc, *CFLAGS, *flags, "-I", RUNTIME, "-c", src, "-o", obj], capture_output=True, text=True)
    return obj, (r.stderr if r.returncode else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", help="llvm-mingw directory (Windows host and Android builds)")
    ap.add_argument("--macos", metavar="LLVM_PREFIX", help="build for macOS arm64 with this LLVM install "
                    "(e.g. /opt/homebrew/opt/llvm@23): lifts with its libLLVM, compiles with its clang")
    ap.add_argument("--native", action="store_true",
                    help="macOS arm64 build with Apple clang from the Command Line Tools (no Homebrew LLVM to compile); "
                    "the lifter's libLLVM comes from --libllvm. Implies the macOS build.")
    ap.add_argument("--libllvm", metavar="DIR_OR_FILE", help="with --native: directory with libLLVM-C.dylib/libLLVM.dylib "
                    "(the launcher bundle's copy), or the library file")
    ap.add_argument("--sdl3-bundle", action="store_true", help="macOS: copy libSDL3 from --sdl3 into <work>/lib and link "
                    "with @executable_path/../lib, so me_pc does not depend on Homebrew")
    ap.add_argument("--base", default="work", help="folder that holds pc/ (the game), pc-deps/ and the build; the module "
                    "table below says work/... (the installer uses ~/MirrorsEdge)")
    ap.add_argument("--work", required=True)
    ap.add_argument("--sdl3", default="/opt/homebrew/opt/sdl3", help="SDL3 prefix for the macOS build")
    ap.add_argument("--regen", action="store_true", help="regenerate the lifted C even if present")
    ap.add_argument("--regen-only", help="comma-separated module tags to regenerate")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--android", metavar="NDK", help="build libmepc.so for Android ARM64 with this NDK "
                    "(the lifted C is generated with --toolchain's LLVM as usual)")
    args = ap.parse_args()
    if args.native:
        if not args.libllvm:
            ap.error("--native needs --libllvm")
        args.macos = "native"
    if not args.toolchain and not args.macos:
        ap.error("--toolchain (Windows/Android) or --macos or --native is required")
    if args.native:
        bindir = args.libllvm
        cc = "/usr/bin/clang"  # the Command Line Tools shim: it also sets the SDK path (the toolchain binary does not)
    elif args.macos:
        bindir = os.path.join(args.macos, "lib")  # what the lifter wants: the directory with libLLVM
        cc = os.path.join(args.macos, "bin", "clang")
    else:
        bindir = os.path.join(args.toolchain, "bin")
        cc = os.path.join(bindir, "x86_64-w64-mingw32-clang.exe")
    extra = ()
    gen_root = os.path.join(args.work, "gen")
    obj_root = os.path.join(args.work, "obj")
    host_file = "rt_host_win.c"
    if args.android:
        cc = os.path.join(args.android, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin", "clang.exe")
        # API 29: native ELF TLS instead of emulated TLS (__emutls_get_address on every
        # rt_thread_self() was ~8% of the game threads on the device; Android 10+ only).
        # ARMv8.4: acquire loads are LDAPR (FEAT_LRCPC), which x86 TSO allows and which is
        # cheaper than LDAR on every guest load; needs such a CPU (lrcpc in /proc/cpuinfo).
        # (macOS arm64 needs no flag: clang's default Apple CPU already has LRCPC.)
        extra = ("--target=aarch64-linux-android29", "-march=armv8.4-a", "-fPIC", "-fno-emulated-tls")
        obj_root = os.path.join(args.work, "obj-android-v84")
        host_file = "rt_host_android.c"
    if args.macos:
        extra = ("-arch", "arm64", "-mmacosx-version-min=12.0", "-I", os.path.join(args.sdl3, "include"), "-DRT_SHATTER")
        obj_root = os.path.join(args.work, "obj-macos")
        host_file = "rt_host_macos.c"
    t0 = time.time()
    global MODULES
    if args.base != "work":
        MODULES = [(t, b.replace("work/", args.base.rstrip("/") + "/", 1), *r) for t, b, *r in MODULES]

    # Modules whose binary is not on this machine are left out (e.g. PhysXCooking.dll: PhysX 2.8.0 names its
    # cooking library NxCooking.dll). The runtime's module table is generated from what remains.
    present = [m for m in MODULES if os.path.exists(m[1])]
    for m in MODULES:
        if m not in present:
            print(f"note: {m[1]} not found, module '{m[0]}' left out")
    MODULES = present

    regen = set(args.regen_only.split(",")) if args.regen_only else None
    for name, binary, cat, *_ in MODULES:
        out = os.path.join(gen_root, name)
        if args.regen or (regen and name in regen) or not os.path.exists(os.path.join(out, f"dispatch_{name}.c")):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_module.py"), binary, "--llvm", bindir,
                            "--catalog", os.path.join(args.work, cat), "--name", name, "--out", out], check=True)

    os.makedirs(gen_root, exist_ok=True)
    write_module_table(os.path.join(gen_root, "rt_modules_gen.c"))
    jobs = [(os.path.join(gen_root, "rt_modules_gen.c"), os.path.join(obj_root, "rt_modules_gen.o"))]
    os.makedirs(obj_root, exist_ok=True)
    for name, *_ in MODULES:
        src_dir = os.path.join(gen_root, name)
        od = os.path.join(obj_root, name)
        os.makedirs(od, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".c"):
                jobs.append((os.path.join(src_dir, f), os.path.join(od, f[:-2] + ".o")))
    od = os.path.join(obj_root, "runtime")
    os.makedirs(od, exist_ok=True)
    for f in ("rt_core.c", "rt_kernel32.c", "rt_sync.c", "rt_files.c", "rt_user32.c", "rt_d3d9.c", "rt_d3d9_gen.c", "rt_physx_support.c", "rt_dinput.c", "rt_ue3.c", "rt_openal.c", "rt_stubs.c", host_file, "rt_main.c"):
        if not os.path.exists(os.path.join(RUNTIME, f)):
            print(f"note: runtime/{f} does not exist yet; build will stop before linking")
            continue
        jobs.append((os.path.join(RUNTIME, f), os.path.join(od, f[:-2] + ".o")))

    if args.macos:
        od = os.path.join(obj_root, "shatter")
        os.makedirs(od, exist_ok=True)
        for f in SHATTER_SOURCES:
            jobs.append((os.path.join(SHATTER, f), os.path.join(od, os.path.basename(f)[:-4] + ".o")))

    def build_job(j):
        if j[0].endswith(".cpp"):
            return compile_shatter(cc + "++", *j, extra)
        return compile_one(cc, *j, extra)

    errors = 0
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for i, (obj, err) in enumerate(ex.map(build_job, jobs)):
            if err:
                errors += 1
                print(f"error in {obj}:\n{err[:3000]}")
            if (i + 1) % 50 == 0:
                print(f"compiled {i + 1}/{len(jobs)} ({time.time() - t0:.0f} s)", flush=True)
    if errors:
        sys.exit(f"{errors} files failed to compile")

    if args.macos:
        if not os.path.exists(os.path.join(RUNTIME, host_file)):
            print(f"compiled {len(jobs)} files without linking: runtime/{host_file} is not written yet")
            return
        exe = os.path.join(args.work, "bin-macos", "me_pc")
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        sdl_lib, sdl_rpath = os.path.join(args.sdl3, "lib"), os.path.join(args.sdl3, "lib")
        if args.sdl3_bundle:
            import shutil
            sdl_lib = os.path.join(args.work, "lib")
            os.makedirs(sdl_lib, exist_ok=True)
            src = os.path.realpath(os.path.join(args.sdl3, "lib", "libSDL3.0.dylib"))
            dst = os.path.join(sdl_lib, "libSDL3.0.dylib")
            shutil.copy2(src, dst)
            os.chmod(dst, 0o755)
            subprocess.run(["install_name_tool", "-id", "@rpath/libSDL3.0.dylib", dst], check=True)
            subprocess.run(["codesign", "--force", "-s", "-", dst], check=True)  # install_name_tool invalidates the signature
            link = os.path.join(sdl_lib, "libSDL3.dylib")  # what -lSDL3 looks for
            if not os.path.lexists(link):
                os.symlink("libSDL3.0.dylib", link)
            sdl_rpath = "@executable_path/../lib"
        # cc++ is clang++ next to clang (Homebrew prefix or the Apple toolchain)
        cxx = cc + "++" if os.path.exists(cc + "++") else os.path.join(os.path.dirname(cc), "clang++")
        subprocess.run([cxx, "-arch", "arm64", *[o for _, o in jobs], "-o", exe, "-Wl,-stack_size,0x4000000",
                        "-L", sdl_lib, "-lSDL3", "-Wl,-rpath," + sdl_rpath, "-lm", "-lpthread"], check=True)
        print(f"built {exe} in {time.time() - t0:.0f} s")
        return
    if args.android:
        lib = os.path.join(args.work, "bin-android", "libmepc.so")
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        rsp = os.path.join(obj_root, "link.rsp")
        with open(rsp, "w") as fp:
            for _, obj in jobs:
                fp.write(obj.replace("\\", "/") + "\n")
        subprocess.run([cc, "--target=aarch64-linux-android29", "-shared", f"@{rsp}", "-o", lib,
                        "-Wl,-z,max-page-size=16384", "-Wl,-Bsymbolic", "-llog", "-landroid", "-ldl", "-lm"], check=True)
        print(f"built {lib} in {time.time() - t0:.0f} s")
        return
    exe = os.path.join(args.work, "bin", "me_pc.exe")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    rsp = os.path.join(obj_root, "link.rsp")
    with open(rsp, "w") as fp:
        for _, obj in jobs:
            fp.write(obj.replace("\\", "/") + "\n")
    subprocess.run([cc, f"@{rsp}", "-o", exe, "-Wl,--stack,8388608", "-lwinmm"], check=True)
    print(f"built {exe} in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
