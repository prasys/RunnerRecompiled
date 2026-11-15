#!/bin/sh
# Builds the distributable disk image: "Mirror's Edge for Mac" (launcher + installer + everything it needs, no game data).
#
#   pc/launcher/macos/make-dmg.sh [out-dir]          default out-dir: work/dmg
#
# Needs (Homebrew, on the build machine only): llvm@23 sdl3 molten-vk vulkan-loader innoextract cabextract msitools,
# the patched DXVK dylib (pc/dxvk/build-macos.sh; or DXVK_DYLIB=<path>), swiftc and iconutil (Xcode CLT).
# Result: <out-dir>/MirrorsEdgeForMac-<VERSION>.dmg  (VERSION from the environment, default "dev")
#
# App layout:  Contents/MacOS/MELauncher
#              Contents/Resources/src/pc/{tools,runtime,shatter}   our code, compiled on the user's Mac
#              Contents/Resources/vendor/{bin,lib,llvm/lib,sdl3,vulkan/icd.d}   third-party pieces, self-contained
#              Contents/Resources/licenses/                        their licences
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${1:-$root/work/dmg}
version=${VERSION:-dev}
dxvk=${DXVK_DYLIB:-$root/work/dxvk/bin/libdxvk_d3d9.0.dylib}
brewp() { brew --prefix "$1"; }
[ -f "$dxvk" ] || { echo "DXVK dylib not found: $dxvk (run pc/dxvk/build-macos.sh or set DXVK_DYLIB)" >&2; exit 1; }

rm -rf "$out/stage" "$out/build"; mkdir -p "$out/stage" "$out/build"
sh "$here/build.sh" "$out/build"
app="$out/stage/Mirror's Edge for Mac.app"
cp -R "$out/build/MirrorsEdgeLauncher.app" "$app"
res="$app/Contents/Resources"; v="$res/vendor"
mkdir -p "$res/src/pc" "$v/bin" "$v/lib" "$v/llvm/lib" "$v/sdl3/lib" "$v/sdl3/include" "$v/vulkan/icd.d" "$res/licenses"

# ---- our code (the lifter and runtime are built on the user's Mac from their own game files) ----
cp -R "$root/pc/tools" "$res/src/pc/tools"; rm -rf "$res/src/pc/tools/__pycache__"
cp -R "$root/pc/runtime" "$res/src/pc/runtime"
cp -R "$root/pc/shatter" "$res/src/pc/shatter"
cp "$root/LICENSE" "$res/licenses/RunnerRecompiled-LICENSE.txt"

# ---- helper tools, with the libraries they need ----
for t in innoextract cabextract msiextract; do
  p=$(command -v "$t") || { echo "$t not installed" >&2; exit 1; }
  cp "$p" "$v/bin/$t"; chmod 755 "$v/bin/$t"
  env "BUNDLE_ORIG_$t=$p" python3 "$here/bundle_dylibs.py" exe "$v/lib" "$v/bin/$t"
done

# ---- LLVM (x86 disassembler for the lifter), SDL3 (headers + library), MoltenVK, Vulkan loader, DXVK ----
python3 "$here/bundle_dylibs.py" lib "$v/llvm/lib" "$(brewp llvm@23)/lib/libLLVM.dylib"
cp "$(brewp sdl3)/lib/libSDL3.0.dylib" "$v/sdl3/lib/"; chmod 755 "$v/sdl3/lib/libSDL3.0.dylib"
install_name_tool -id @rpath/libSDL3.0.dylib "$v/sdl3/lib/libSDL3.0.dylib"; codesign --force -s - "$v/sdl3/lib/libSDL3.0.dylib"
ln -sf libSDL3.0.dylib "$v/sdl3/lib/libSDL3.dylib"
cp -R "$(brewp sdl3)/include/SDL3" "$v/sdl3/include/SDL3"
python3 "$here/bundle_dylibs.py" lib "$v/lib" "$(brewp molten-vk)/lib/libMoltenVK.dylib" \
    "$(brewp vulkan-loader)/lib/libvulkan.1.dylib" "$dxvk"
cat > "$v/vulkan/icd.d/MoltenVK_icd.json" <<'J'
{ "file_format_version": "1.0.0", "ICD": { "library_path": "../../lib/libMoltenVK.dylib", "api_version": "1.2.0", "is_portability_driver": true } }
J

# ---- licences of what is bundled ----
lic() { # name prefix-dir...
  n=$1; shift; mkdir -p "$res/licenses/$n"
  for d in "$@"; do find "$d" -maxdepth 3 \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -type f 2>/dev/null \
    | while read -r f; do cp "$f" "$res/licenses/$n/$(basename "$f")"; done; done
  [ -n "$(ls "$res/licenses/$n")" ] || echo "warning: no licence file found for $n" >&2
}
lic LLVM "$(brewp llvm@23)"; lic SDL3 "$(brewp sdl3)"; lic MoltenVK "$(brewp molten-vk)"
lic Vulkan-Loader "$(brewp vulkan-loader)"; lic innoextract "$(brewp innoextract)"
lic cabextract "$(brewp cabextract)"; lic msitools "$(brewp msitools)"
mkdir -p "$res/licenses/DXVK"; [ -f "$(dirname "$dxvk")/../dxvk-3.1.1-git/LICENSE" ] && cp "$(dirname "$dxvk")/../dxvk-3.1.1-git/LICENSE" "$res/licenses/DXVK/LICENSE" || true
cp "$root/pc/shatter/LICENSE" "$res/licenses/Shatter-LICENSE.txt" 2>/dev/null || true
cat > "$res/licenses/README.txt" <<'T'
This app bundles third-party software. Their licences are in the folders here.
cabextract and msitools are GPL/LGPL programs; their source code is available from
  https://www.cabextract.org.uk/   and   https://gitlab.gnome.org/GNOME/msitools
innoextract (zlib licence): https://constexpr.org/innoextract/
DXVK (zlib, patched; patch in this app's source repository): https://github.com/doitsujin/dxvk
Nothing from the game Mirror's Edge is included. You need your own copy.
T

# ---- checks: nothing may point at Homebrew; the helper tools and LLVM must load ----
bad=$(find "$v" -type f \( -perm -u+x -o -name '*.dylib' \) -exec sh -c 'otool -L "$1" 2>/dev/null | grep -l "/opt/homebrew\|/usr/local" >/dev/null && echo "$1"' _ {} \; | head)
[ -z "$bad" ] || { echo "Homebrew paths remain in:" >&2; echo "$bad" >&2; exit 1; }
for t in innoextract cabextract msiextract; do env -i "$v/bin/$t" --version >/dev/null 2>&1 || env -i "$v/bin/$t" --help >/dev/null 2>&1 || { echo "$t does not run from the bundle" >&2; exit 1; }; done
env -i /usr/bin/python3 -I -c "
import ctypes,sys
l=ctypes.CDLL(sys.argv[1])
for n in ('LLVMInitializeX86TargetInfo','LLVMInitializeX86TargetMC','LLVMInitializeX86Disassembler'): getattr(l,n)()
print('libLLVM loads and has the x86 disassembler')" "$v/llvm/lib/libLLVM.dylib"

# ---- sign (ad hoc), package ----
codesign --force --deep -s - "$app"
ln -s /Applications "$out/stage/Applications"
cat > "$out/stage/READ ME FIRST.txt" <<'T'
1. Drag "Mirror's Edge for Mac" into Applications.
2. Open it. macOS may say it cannot verify the app (it is not signed by a paid Apple developer account):
   right-click the app, choose Open, then Open again. If that does not offer Open, go to System Settings >
   Privacy & Security and choose "Open Anyway".
3. Follow the steps in the app: you need your own GOG copy of Mirror's Edge (version 1.01).
Needs a Mac with Apple Silicon and macOS 15 or later, and about 15 GB of free space.
T
dmg="$out/MirrorsEdgeForMac-$version.dmg"; rm -f "$dmg"
hdiutil create -volname "Mirror's Edge for Mac" -srcfolder "$out/stage" -ov -format UDZO "$dmg" >/dev/null
echo "built $dmg ($(du -h "$dmg" | cut -f1))"
