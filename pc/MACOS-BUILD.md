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
brew install meson ninja pkg-config sdl3 vulkan-loader molten-vk   # if not installed yet
pc/dxvk/build-macos.sh
```

The script clones DXVK v3.1.1 (with submodules) into `work/dxvk/dxvk-3.1.1-git`, applies
`pc/dxvk/dxvk-3.1.1-macos.patch`, configures Meson and builds (about 70 s on an M1 Max). The result is **copied** to
`work/dxvk/bin/libdxvk_d3d9.0.dylib`, so a later rebuild does not replace a file a running game has open. Pass another
work directory as the first argument. If the patch changes, delete `work/dxvk/dxvk-3.1.1-git` and run the script again
(it refuses to patch a tree it has already patched with a different patch).

The patch contains the macOS fixes: the SDL3 window system, the Vulkan loader path, depth-compare emulation for MoltenVK
(text and colours), deferred resource destroy (without it the GPU device is lost on level loads), and the pipeline
warm-up (see "Pipeline warm-up" below). Details are in the plan.

## 6. Build the game

```sh
python3 -I pc/tools/build_pc_runtime.py --macos /opt/homebrew/opt/llvm@23 --work work/pc-recomp
```

This lifts every module to C and compiles it. It took 7-8 minutes on an M1 Max. Later runs only rebuild what changed;
add `--regen` after updating the lifter (`pc/tools/*.py`). Output: `work/pc-recomp/bin-macos/me_pc`.

## 7. Run

```sh
mkdir -p work/pc-user
ME_PC_DXVK=$PWD/work/dxvk/bin/libdxvk_d3d9.0.dylib \
  work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```

- `work/pc-user` gets what the game writes: its settings and saves.
- The first start is silent for about 17 s while the game loads its shaders and packages.
- The mouse is captured while the window has focus.
- More options (logging, frame dumps, vsync) are in the table in `AGENTS.md`, "Quick start".

Both binaries are build outputs under `work/` (not in the repository): the runtime `work/pc-recomp/bin-macos/me_pc` (step 6)
and the DXVK copy `work/dxvk/bin/libdxvk_d3d9.0.dylib` (step 5). The launcher looks for them there.

## Pipeline warm-up (level-load stalls)

MoltenVK has no pipeline libraries, so every new render-state/shader combination compiles on the game thread (50-300 ms
each on real levels). The DXVK patch records each combination per map and replays it on the next visit:

- Files: `~/.cache/dxvk/pipelines.warm` (records: map, two shader names, raw pipeline state) and `pipelines.warm.shaders`
  (the shaders' bytecode, copied from the game while it runs). Both are written by playing; nothing from the game is
  shipped in this repository.
- The runtime tells DXVK which map loads (the persistent `*_p.umap` / `*_p_LOC_int.upk` / `TdMainMenu` opening). DXVK
  re-creates that map's shaders from the saved bytecode on a helper thread, so the pipelines compile while the game
  loads, and holds the present for the rest (about 0.6 s measured) once the scene draws. A map benefits from its second
  visit on; the first visit is a cold run.
- Cold start for testing: the launcher's Game tab has "Clear warm-up data (cold start)" (optionally with DXVK's own
  shader cache), or delete `~/.cache/dxvk/pipelines.warm*` by hand. `DXVK_PIPELINE_WARM=0` turns it off.
- Knobs (environment): `DXVK_PIPELINE_WARM_FILE` (file path), `DXVK_PIPELINE_WARM_EARLY=0` (no early shader creation),
  `DXVK_PIPELINE_WARM_HOLD_DRAWS` (hold only when the frame has at least this many draws; default 30, 0 = also on loading
  screens), `DXVK_PIPELINE_WARM_HOLD_MIN` (hold only for at least this many queued tasks; default 20),
  `DXVK_PIPELINE_WARM_WAIT_MS` (cap on one hold; default 10000).
- Other stall tools: `ME_PC_LOG_WAITS=<ms>` logs long guest waits with the caller address; the engine's fixed 1 s sleep
  before the rendering thread restarts is skipped (`ME_PC_KEEP_SLEEP1000=1` keeps it).
- Background and measurements: GitHub issues #1, #9, #10 (shipping recorded files), #11 (record all maps), #12 (load a
  named map).

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
