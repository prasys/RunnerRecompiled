"""Builds RunnerBuilder.exe: the window (RunnerBuilder.cs) with the project sources embedded.

  python -I outputs/mirrors-edge-native/pc/tools/builder-gui/make_exe.py --dxvk <libdxvk_d3d9.so> \
      --adrenotools <folder> [--turnip <turnip-*.zip>] --out <folder>

The payload has no game code or data: only sources from this repository and prebuilt third-party
libraries (DXVK with the patches in pc/dxvk, adrenotools, Mesa Turnip), so the exe can be shared.
"""
import argparse
import os
import subprocess
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
CSC = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Microsoft.NET", "Framework64", "v4.0.30319", "csc.exe")
ADRENO = ("libadrenotools.so", "libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
TREES = ("pc/runtime", "android/native", "android/res", "android/src")
FILES = ("android/AndroidManifest.xml", "android/build.ps1", "pc/dxvk/THIRD-PARTY.md", "LICENSE")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dxvk", required=True)
    ap.add_argument("--adrenotools", required=True)
    ap.add_argument("--turnip")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keep-payload", action="store_true", help="leave payload.zip next to the exe (tests)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    payload = os.path.join(args.out, "payload.zip")
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        tools = os.path.join(REPO, "pc", "tools")
        for name in sorted(os.listdir(tools)):
            if name.endswith(".py"):
                z.write(os.path.join(tools, name), f"pc/tools/{name}")
        for tree in TREES:
            for base, _, names in os.walk(os.path.join(REPO, *tree.split("/"))):
                for name in names:
                    path = os.path.join(base, name)
                    z.write(path, os.path.relpath(path, REPO).replace(os.sep, "/"))
        for name in FILES:
            path = os.path.join(REPO, *name.split("/"))
            if os.path.isfile(path):
                z.write(path, name)
        z.write(args.dxvk, "prebuilt/libdxvk_d3d9.so")
        for name in ADRENO:
            z.write(os.path.join(args.adrenotools, name), f"prebuilt/adrenotools/{name}")
        if args.turnip:
            z.write(args.turnip, f"prebuilt/{os.path.basename(args.turnip)}")
    exe = os.path.join(args.out, "RunnerBuilder.exe")
    subprocess.run([CSC, "/nologo", "/target:winexe", "/optimize+", "/codepage:65001", f"/out:{exe}",
                    f"/resource:{payload},payload.zip", "/r:System.Windows.Forms.dll", "/r:System.Drawing.dll",
                    "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll",
                    os.path.join(HERE, "RunnerBuilder.cs")], check=True)
    if not args.keep_payload:
        os.remove(payload)
    print(f"{exe}: {os.path.getsize(exe) / 2 ** 20:.1f} MB")


if __name__ == "__main__":
    main()
