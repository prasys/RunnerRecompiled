#!/bin/sh
# Builds the patched DXVK (libdxvk_d3d9.0.dylib) for macOS arm64 from pc/dxvk/dxvk-3.1.1-macos.patch.
# usage: pc/dxvk/build-macos.sh [work-dir]      (default: work/dxvk)
# Needs: git, patch, meson, ninja, pkg-config, SDL3, Vulkan loader (Homebrew), Xcode command line tools.
#   brew install meson ninja pkg-config sdl3 vulkan-loader molten-vk
# Result: <work-dir>/bin/libdxvk_d3d9.0.dylib  (a copy: rebuilding does not touch a copy a running game has open)
# Point the game at it with ME_PC_DXVK=<that path>, or let the launcher find work/dxvk/bin/.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${1:-$root/work/dxvk}
src=$work/dxvk-3.1.1-git
patchfile=$here/dxvk-3.1.1-macos.patch
mkdir -p "$work/bin"
if [ ! -d "$src" ]; then
  git clone --recursive --branch v3.1.1 https://github.com/doitsujin/dxvk.git "$src"
fi
cd "$src"
if [ ! -f .macos-patch-applied ]; then
  if ! git diff --quiet || ! git -C subprojects/dxbc-spirv diff --quiet; then
    echo "$src has local changes but no .macos-patch-applied marker; use a fresh checkout." >&2; exit 1
  fi
  patch -p1 < "$patchfile"
  cp "$patchfile" .macos-patch-applied
elif ! cmp -s "$patchfile" .macos-patch-applied; then
  echo "The patch changed since this tree was patched. Remove $src and run again (or apply the difference by hand)." >&2; exit 1
fi
if [ ! -d build-macos ]; then
  CPPFLAGS=-I/opt/homebrew/include CXXFLAGS=-I/opt/homebrew/include meson setup build-macos --buildtype=release \
    -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
    -Dnative_sdl2=disabled -Dnative_sdl3=enabled -Dnative_glfw=disabled
fi
ninja -C build-macos
cp build-macos/src/d3d9/libdxvk_d3d9.0.dylib "$work/bin/libdxvk_d3d9.0.dylib"
echo "built $work/bin/libdxvk_d3d9.0.dylib"
