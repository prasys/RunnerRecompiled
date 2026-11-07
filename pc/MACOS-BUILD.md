# Building and running the PC version on macOS (Apple Silicon)

This builds the lifted Windows PC version of *Mirror's Edge* as a native arm64 macOS program. Direct3D 9 goes through
DXVK on MoltenVK. You need your own copy of the game from GOG. Nothing from the game is in this repository, and nothing
you build from it may be shared: game files, the lifted C, catalogs, rebased DLLs and the built binary all stay in
your `work/` folder (it is gitignored).

Status and known problems are at the end. Developer notes are in `AGENTS.md` and `pc/MACOS-PORT-PLAN.md`.

## Requirements

- A Mac with Apple Silicon. Tested only on an M1 Max with macOS 15; other versions are untested.
- About 15 GB of free disk space: the extracted game is 11 GB, the build about 1.1 GB (lifted C, objects, binary),
  DXVK about 0.25 GB, plus the Homebrew packages.
- Xcode Command Line Tools (`xcode-select --install`), and Python 3 (the one that comes with them is fine).
- [Homebrew](https://brew.sh) in its default Apple Silicon location, `/opt/homebrew`. The runtime looks for MoltenVK,
  the Vulkan loader and SDL3 there.
- The GOG installer of *Mirror's Edge*, version 1.01 (`setup_mirrors_edge_1.01_jpfix_(41238).exe` with its `-1.bin`,
  `-2.bin`, `-3.bin` parts). The lifted code is tied to this exact build: the game executable must have SHA-256
  `58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd`.

```sh
brew install llvm@23 sdl3 molten-vk vulkan-loader vulkan-headers glslang meson ninja pkgconf \
             innoextract cabextract msitools
```

All commands below run from the root of this repository, on branch `macos-port`.

## 1. Extract the game

```sh
mkdir -p work/pc
innoextract -d work/pc ~/Downloads/'setup_mirrors_edge_1.01_jpfix_(41238).exe'
shasum -a 256 work/pc/Binaries/MirrorsEdge.exe     # must print 58de3df2...a3dd
```

The GOG installer ships the real PhysX 2.8 core as `PhysXCore.bak`. The game and the build need it under its real name:

```sh
cp work/pc/Binaries/PhysXCore.bak work/pc/Binaries/PhysXCore.dll
```

## 2. The Microsoft C runtime (msvcr80, msvcp80)

The game uses the Visual C++ 2005 SP1 runtime (8.0.50727.762). It is lifted like the game, so the real DLLs are
needed. They are inside the redistributable that comes with the GOG installer:

```sh
mkdir -p work/x/vcredist work/pc-deps
cabextract -q -d work/x/vcredist work/pc/__redist/MSVC2005/vcredist_x86.exe
cabextract -q -d work/x/vcredist/u work/x/vcredist/VCREDI~3.EXE
msiextract -C work/x/vcredist/u/msi work/x/vcredist/u/vcredist.msi
cp work/x/vcredist/u/msi/Windows/system32/msvcr80.dll work/x/vcredist/u/msi/Windows/system32/msvcp80.dll work/pc-deps/
shasum -a 256 work/pc-deps/*.dll
# msvcp80.dll 2026f3c4f830dff6883b88e2647272a52a132f25eb42c0d423e36b3f65a94d08
# msvcr80.dll 02bdde38e4c6bd795a092d496b8d6060cdbe71e22ef4d7a204e3050c1be44fa9
```

`work/x` can be deleted afterwards.

## 3. Rebase the game's DLLs

The PhysX, audio and Bink DLLs all ask for the same load address. Each gets a rebased copy at its own address, and
those copies are what gets lifted. The addresses must match the module table in `pc/tools/build_pc_runtime.py`.

```sh
mkdir -p work/pc-deps/rebased
B=work/pc/Binaries; R=work/pc-deps/rebased
for m in PhysXLoader:20000000 PhysXCore:21000000 NxCooking:23000000 PhysXExtensions:24000000 \
         NxCharacter:25000000 ogg:26000000 vorbis:27000000 vorbisfile:28000000 vorbisenc:29000000 \
         libresample:2a000000 binkw32:2b000000; do
  python3 -I pc/tools/rebase_pe.py "$B/${m%%:*}.dll" "$R/${m%%:*}.dll" "${m##*:}"
done
```

(`PhysXCooking.dll` does not exist in this PhysX version: address `22000000` stays unused, and the build skips it.)

## 4. Function catalogs

The lifter needs a catalog of every function in each binary. The game's catalog goes in the work folder itself, and
each DLL's catalog in a subfolder named after it:

```sh
W=work/pc-recomp; L=/opt/homebrew/opt/llvm@23/lib
python3 -I pc/tools/catalog.py work/pc/Binaries/MirrorsEdge.exe --llvm $L --out $W
python3 -I pc/tools/catalog.py work/pc-deps/msvcr80.dll --llvm $L --out $W/msvcr80
python3 -I pc/tools/catalog.py work/pc-deps/msvcp80.dll --llvm $L --out $W/msvcp80
for m in PhysXLoader PhysXCore NxCooking PhysXExtensions NxCharacter ogg vorbis vorbisfile vorbisenc libresample binkw32; do
  t=$(echo $m | tr A-Z a-z)
  python3 -I pc/tools/catalog.py work/pc-deps/rebased/$m.dll --llvm $L --out $W/$t
done
```

For reference, the game's catalog has about 178,600 functions (`work/pc-recomp/summary.json`).

## 5. DXVK for macOS

```sh
mkdir -p work/dxvk
git clone --recursive --branch v3.1.1 https://github.com/doitsujin/dxvk.git work/dxvk/dxvk-3.1.1-git
cd work/dxvk/dxvk-3.1.1-git
patch -p1 < ../../../pc/dxvk/dxvk-3.1.1-macos.patch
CPPFLAGS=-I/opt/homebrew/include CXXFLAGS=-I/opt/homebrew/include meson setup build-macos --buildtype=release \
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
  -Dnative_sdl2=disabled -Dnative_sdl3=enabled -Dnative_glfw=disabled
ninja -C build-macos
cd ../../..
```

The result is `work/dxvk/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib`. The patch contains the macOS
fixes: the SDL3 window system, the Vulkan loader path, depth-compare emulation for MoltenVK (text and colours), and
deferred resource destroy (without it the GPU device is lost on level loads). Details are in the plan.

## 6. Build the game

```sh
python3 -I pc/tools/build_pc_runtime.py --macos /opt/homebrew/opt/llvm@23 --work work/pc-recomp
```

This lifts every module to C and compiles it. It took 7-8 minutes on an M1 Max. Later runs only rebuild what changed;
add `--regen` after updating the lifter (`pc/tools/*.py`). Output: `work/pc-recomp/bin-macos/me_pc`.

## 7. Run

```sh
mkdir -p work/pc-user
ME_PC_DXVK=$PWD/work/dxvk/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib \
  work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```

- `work/pc-user` gets what the game writes: its settings and saves.
- The first start is silent for about 17 s while the game loads its shaders and packages.
- The mouse is captured while the window has focus.
- More options (logging, frame dumps, vsync) are in the table in `AGENTS.md`, "Quick start".

Copy the DXVK dylib somewhere else if you plan to rebuild DXVK while the game runs: a rebuild replaces the file under
the running game.

## Launcher (optional)

`pc/launcher/macos/build.sh work` builds `work/MirrorsEdgeLauncher.app` (SwiftUI, needs `swiftc`). It fills in the paths
from `work/`, then lets you set: resolution (`ResX`/`ResY` in `TdEngine.ini`), render scale (the game renders at 25-100%
of the window, DXVK scales up), vsync, frame limit and anisotropy (via `DXVK_CONFIG`), Metal HUD, DXVK HUD, PhysX on/off
(`PhysXEnhanced`, `ME_PC_SHATTER`, `PXSHIM_CLOTH`), field of view, mouse capture, guest CPUs, controller button mapping
and inverted look, log path and extra environment. Settings are saved in
`~/Library/Application Support/RunnerRecompiled/launcher.json`; **Play** writes `TdEngine.ini` (backup
`TdEngine.ini.launcher-bak`) and starts `me_pc`. Headless: `MELauncher --print` (shows the command and environment),
`--apply-ini`, `--launch`, `--snapshot out.png` (renders the banner only). The header banner cycles through images from
`~/Library/Application Support/RunnerRecompiled/backdrops/` (png/jpg/heic/tiff; the folder can be changed in Paths) and
falls back to built-in generated skyline art; no game art is shipped. `build.sh` also draws the glass-city app icon
(`make-icon.swift` -> `AppIcon.icns`). The **Upscaling** picker (Native, Standard, MetalFX) sets `ME_PC_UPSCALER`; MetalFX is experimental and needs the DXVK build
from `pc/dxvk/dxvk-3.1.1-macos-metalfx.patch` (Paths tab, "DXVK (MetalFX)"; the launcher finds
`work/dxvk-metalfx/build-spike/...`), see `pc/spikes/macos-metalfx/README.md`. Not in it yet: the frame cap above 62 fps (#5), FSR, anti-aliasing (set in
the game's Options > Video), "start on the full game".

## PhysX effects (optional)

The enhanced PhysX effects (glass, debris, smoke, cloth) are off by default in the game's settings. Turn on PhysX in
the game's graphics options (or set `PhysXEnhanced=True` in `TdEngine.ini`). The macOS build runs these effects on
[Shatter](https://github.com/Arecsu/shatter)'s native solvers (`pc/shatter/`), not on the lifted PhysX core, which is
too slow for them. `ME_PC_SHATTER=0` turns that off; `PXSHIM_CLOTH=0` leaves only cloth to the lifted core.

## Status and known problems

Works:
- the title screen and menus;
- the tutorial level at 60 fps with keyboard and mouse;
- later levels (user play, about 17 minutes over two sessions), with the PhysX effects on;
- quitting from the menu.

Known problems:
- **Long stall when a level starts.** The first frame of a level can take 20+ seconds the first time, while the
  pipelines are built (prasys/RunnerRecompiled#1). Wait it out; it is much shorter on later runs.
- Short hitches (about 1 s) while moving through a level for the first time.

Not checked yet:
- game controllers;
- audio quality;
- the startup movie;
- the whole game to the end.

Problems are welcome as GitHub issues on `prasys/RunnerRecompiled`. Include the console output and, if the game
froze, any `mvk-error` line.
