# AGENTS.md

Shared context for coding agents (Claude Code, Codex, OpenCode, and others) working in this repo.
Last updated 2026-11-06. `CLAUDE.md` is the index of all docs.

## What this repo is

RunnerRecompiled: a fan-made static recompilation of *Mirror's Edge* (2008). Nothing from the game is in the repo;
users supply their own copy.

- `xbox/`, `android/`: Xbox 360 build, PowerPC lifted with XenonRecomp, own kernel/XAM runtime, patched RexGlue Vulkan
  GPU backend, Android ARM64 app.
- `pc/`: **the Windows PC build**: x86-32 lifted to C by our own lifter, our own Win32 runtime, and a Direct3D 9
  bridge to DXVK Native. Hosts: Windows x64 (bring-up), Android ARM64, **macOS arm64 (this effort)**.

Most older notes are in Russian. New notes are English.

## Status of the macOS port (2026-11-06)

| Phase | State |
|---|---|
| 0 tools and GOG files | done |
| 1 lifter and build on macOS | done: full lift (185,486 game functions) and compile, 0 errors |
| 2 address-space spike | done: findings drive the host memory design |
| 3 `rt_host_macos.c` | done: links, runs |
| 4 DXVK on MoltenVK | done: the window shows the picture (needed a DXVK main-thread surface fix) |
| 5 bring-up | **in progress**: title, menu and the tutorial level render and play at 60 fps with keyboard/mouse (session 2 fixed text/colours, double-counted mouse, tearing/judder via vsync, a Netbios crash). **Open: GPU device lost ("Invalid Resource") during some level loads, which freezes the game; one-frame wrong-camera flashes while walking.** Next steps: plan, "Still open (next session starts here)" |
| 6 packaging | open |

Fixed in session 2: text drawn as boxes and the grey/washed-out colours had one cause, SPIRV-Cross typing textures as
Metal depth textures (plan, "Phase 5 findings, session 2"). **The DXVK build in `work/dxvk-mac` predates that fix**:
rebuild it with the current patch, or use `work/dxvk-textfix/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib`.
Not yet checked: controller in the menu, audio, the startup movie, the tutorial level, PhysX under play.
The fps figure is probably the game's own cap, not our headroom.
Details and numbers: `pc/MACOS-PORT-PLAN.md`.

## Hard rules

- **Never commit game data or anything derived from it.** `work/`, `private/`, `build/`, `*.xex`, `*.iso`, `*.bin`,
  `*.tfc` are gitignored. Lifted C, catalogs, rebased DLLs, extracted installers, DXVK builds stay in `work/`.
- The lifter, runtime and scripts are original code (MIT). Do not copy code from other recomp projects, whatever their
  license; credit ideas. (Prior art notes in the plan list licenses; several projects are all-rights-reserved.)
- Do not run or modify the user's Downloads (`*.iso`, `*.bin`, installers). Read-only.
- Do not touch the Xbox/Android code paths unless the task says so.
- Do not commit or push unless asked. Remotes: `origin` = upstream (`singlemate/RunnerRecompiled`, not the user's);
  `fork` = the user's public fork (`prasys/RunnerRecompiled`), macOS work goes to branch `macos-port`.
- Report what was measured and what was only assumed. A log line that says "frames presented" is not "looks right".

## Quick start (macOS, from the project root)

```sh
# 1. build (lifts and compiles everything not up to date; the first full run takes ~8 min)
python3 -I pc/tools/build_pc_runtime.py --macos /opt/homebrew/opt/llvm@23 --work work/pc-recomp
# 2. run (opens a window; the game is quiet for ~17 s while it loads shaders and packages)
export ME_PC_DXVK=$PWD/work/dxvk-mac/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib
work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```

Environment variables (all optional unless marked):

| Variable | Effect |
|---|---|
| `ME_PC_DXVK` | **needed**: path to `libdxvk_d3d9*.dylib` |
| `ME_PC_FRAME_LOG=0` | turn off the once-a-second `D3D9 frames: ...` log line |
| `ME_PC_MOUSE_CAPTURE=0` | do not capture the mouse (relative mode) while the window has focus |
| `ME_PC_LOG_FILES=1`, `ME_PC_LOG_INPUT=1` | log file opens / input |
| `ME_PC_DUMP_FRAMES=300,900,2990-3400/2`, `ME_PC_DUMP_DIR=<dir>` | write the D3D9 backbuffer as `frame_<n>.ppm` before those Presents and log non-black share and mean RGB (frames or ranges `first-last[/step]`, up to 16 entries) |
| `ME_PC_TRACE_FRAME=<n>`, `ME_PC_TRACE_MAX`, `ME_PC_TRACE_DUMP=1` | log every draw of frame n (shaders, render target, blend/alpha/sRGB states, bound textures and sampler states); with `DUMP`, also write shader bytecode and 8-bit textures to `ME_PC_DUMP_DIR` |
| `ME_PC_AUTO_KEYS=32,40` | tap Return at those seconds after start (gets an unattended run past "Press Any Key"; add `ME_PC_ASSUME_FOCUS=1`). `32,36,40,44,48` starts a new game and loads the tutorial |
| `ME_PC_AUTO_INPUT="3010:mouse=400,0,20;3410:down=W;3500:up=W"` | scripted input keyed to frame numbers: mouse motion spread over N frames, key down/up by SDL key name; the window counts as in front without capturing the real cursor (macOS) |
| `ME_PC_VSYNC=0` | macOS: keep the game's no-vsync present interval (default: vsync, see the plan) |
| `ME_PC_LOG_QUERIES=1` | per-second occlusion-query stats (GetData calls, not ready, zero pixels, visible) |
| `MVK_CONFIG_LOG_LEVEL=2`, `MTL_DEBUG_LAYER=1` (+ `MTL_DEBUG_LAYER_ERROR_MODE=nslog` or `assert`) | MoltenVK warnings/errors (the reason for a lost device); Metal API validation (names the bad resource; `assert` leaves a crash report with the stack in `~/Library/Logs/DiagnosticReports`) |
| `ME_PC_LOG_PATH` | also where DXVK's log goes, unless `DXVK_LOG_PATH` is set |
| `DXVK_LOG_LEVEL=info`, `DXVK_LOG_PATH=<dir>` | DXVK's own log |
| `SDL_VULKAN_LIBRARY`, `VK_ICD_FILENAMES`, `DXVK_WSI_DRIVER` | set by the host if unset (Homebrew paths, SDL3) |

Rebuild after editing only `rt_*.c`: the same command (it recompiles what changed).

## Local layout (this Mac)

`work/` exists only in the main checkout, `/Users/pradeesh/orca/workspaces/RunnerRecompiled/stingaree/work/`. Other
worktrees do not have it; use that absolute path (read-only for inputs).

| Path | Contents |
|---|---|
| `work/pc/` | GOG install extracted with `innoextract` from `~/Downloads/setup_mirrors_edge_1.01_jpfix_(41238).exe`: `Binaries/`, `TdGame/`, `Engine/` |
| `work/pc-deps/` | `msvcr80.dll`, `msvcp80.dll` (VC 2005 redistributable) |
| `work/pc-deps/rebased/` | 11 DLLs rebased by `pc/tools/rebase_pe.py` to `0x20000000 + n*0x01000000` in the build script's order: PhysXLoader 20, PhysXCore 21, (PhysXCooking 22, absent), NxCooking 23, PhysXExtensions 24, NxCharacter 25, ogg 26, vorbis 27, vorbisfile 28, vorbisenc 29, libresample 2a, binkw32 2b |
| `work/pc-recomp/` | per-module catalogs, `gen/` (lifted C, ~590 MB), `obj-macos/`, `bin-macos/me_pc` |
| `work/pc-user/` | what the game writes (config, saves) |
| `work/dxvk-mac/` | DXVK 3.1.1 sources and the macOS build |
| `work/x/` | scratch unpacking of the redistributables (can be deleted) |
| `work/*.log`, `work/dxvk-logs/` | run logs |

Facts to rely on:
- `MirrorsEdge.exe` SHA-256 `58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd`. Catalog addresses are
  tied to this exact build.
- The GOG install renames `PhysXCore.dll` to `PhysXCore.bak` (real 2.8.0 FC7 DLL); `work/pc/Binaries` has a copy named
  `PhysXCore.dll`. The game ships its own `NxCooking`, `PhysXLoader`, `PhysXExtensions`, `NxCharacter` (differ from the
  9.17 System Software copies; use the game's).
- PhysX 2.8.0 has no `PhysXCooking.dll`; the build script skips modules whose binary is missing. Nothing has asked for it yet.
- The lifted game is `-O2`, x87 as `double`, 68 game functions contain traps (unsupported instructions).

## Lessons learned (read before debugging)

- **Memory:** host page is 16 KB. Commit rounds to 16 KB; decommit is a `MAP_FIXED` anonymous remap (fresh zero pages).
  `MADV_DONTNEED` does not zero on macOS; do not use it. Faults on `PROT_NONE` arrive as **SIGBUS**; handle both.
  `VirtualProtect` is a no-op in the runtime, so no software page table is needed. (`pc/spikes/macos-addrspace/`)
- **SDL3 snapshots the environment at `SDL_Init`.** A later `setenv()` (e.g. `SDL_VULKAN_LIBRARY`) is ignored. Set
  such variables in `main()` before `SDL_Init`. Symptom of getting this wrong: `Direct3DCreate9` aborts with an
  uncaught `dxvk::DxvkError` and no message.
- **Black window, picture fine in the backbuffer:** SDL creates the Metal view/layer inside `SDL_Vulkan_CreateSurface`,
  and DXVK calls it from the game thread. Fixed in the DXVK patch with `SDL_RunOnMainThread`; it only works while the
  main thread pumps SDL events (it does). Debug with the backbuffer dump (`ME_PC_DUMP_FRAMES`), not with screenshots.
- **Threads:** Cocoa needs the event loop on the main thread. `rt_host_macos.c` has the real `main()` (SDL loop); the
  game runs on a worker via `me_pc_main` (`rt_main.c`'s `main`, renamed with `-Dmain=me_pc_main` for macOS/Android).
  The game gets the `SDL_Window*` as its HWND (DXVK's SDL back end casts it).
- **The build cache is keyed on file times, not flags.** After changing compile flags, delete the stale `.o`
  (e.g. `work/pc-recomp/obj-macos/runtime/rt_main.o` after adding `-Dmain=me_pc_main`: symptom `_me_pc_main` undefined).
- **The differential harness cannot run on Apple Silicon** (it executes the original x86-32 code natively in a 32-bit
  Windows process). The catalog itself reproduces the Windows numbers (tools at `4f2d67e`: 178,605 functions;
  current tools: 185,536). Alternatives for later: a Windows machine or an x86 emulator as the reference.
- **Generated files:** `rt_d3d9_gen.c` / `rt_d3d9_idx.h` come from `pc/tools/gen_d3d9_bridge.py` and DXVK's `d3d9.h`
  (`work/dxvk-mac/.../include/native/directx/d3d9.h`). Regeneration was verified byte-identical before editing; change
  the generator, not the output.
- **Shell on macOS:** there is no `timeout`; run in the background and `kill` after a sleep. Do not `sleep` more than
  ~45 s in one tool call. `lldb` hung on this binary and `sample` stalled; neither is a reliable tool here.
  `screencapture` from the terminal showed only the wallpaper (screen-recording permission): do not trust it for
  "is it drawing"; use the frame counter.
- **Line endings:** several runtime files are CRLF (`rt_d3d9.c`, `rt_d3d9.h`, `gen_d3d9_bridge.py`). Edit them
  preserving CRLF (Python: `open(p, newline='')`), or diffs against upstream balloon.
- **Exit code 137 after ~60-70 s** has been seen twice with frame dumping on and is unexplained (see the plan).
- **Wrong texture colours on MoltenVK: read the MSL.** `MVK_CONFIG_SHADER_DUMP_DIR=<dir>` dumps the Metal source
  MoltenVK compiles; `DXVK_SHADER_DUMP_PATH=<dir>` dumps DXVK's SPIR-V and the D3D9 bytecode (`spirv-cross --vulkan-semantics`
  makes it readable). A texture declared `depth2d<float>` in MSL returns `(r, r, r, r)`; that was the text-box bug.
  Match a shader to a draw by comparing its bytecode with `ME_PC_TRACE_DUMP` output.
- **DXVK 3.1 shader cache:** `~/.cache/dxvk/*.dxvk.bin` stores converted shader IR keyed on the shader key. A DXVK patch
  that changes shader conversion must change the key (e.g. a field in `D3D9ShaderOptions`), or test with
  `DXVK_SHADER_CACHE=0` / a fresh `DXVK_SHADER_CACHE_PATH`; shader dumping also turns the cache off.
- **Frozen window with frames still counting = lost GPU device.** Look for `VK_ERROR_DEVICE_LOST` in the run log; run
  with `MVK_CONFIG_LOG_LEVEL=2` for Metal's reason. Force-quitting it gives exit code 137.
- **Do not guess at DXVK failures:** read its log (`DXVK_LOG_LEVEL=info`) and source (`work/dxvk-mac/.../src`) first.

## Tools installed (Homebrew, arm64, macOS 15.8)

`llvm@23`, `llvm`, `molten-vk`, `vulkan-headers`, `vulkan-loader`, `glslang`, `spirv-cross`, `sdl3`, `sdl2`, `ninja`,
`meson`, `openal-soft` (not needed: audio uses SDL3), `innoextract`, `sevenzip` (`7zz`), `cabextract`, `msitools`. No Wine.

Python scripts run isolated: `python3 -I pc/tools/<script>.py ...`. Older docs show
`outputs/mirrors-edge-native/pc/tools/...` paths from a different tree; here it is `pc/tools/...`.
Lifter tools take `--llvm /opt/homebrew/opt/llvm@23/lib` (a directory with `libLLVM*.dylib`).

## Working with Orca (agent delegation)

This repo is worked on inside Orca. Use the `orca` CLI (skill `orca-cli`; `orca skills get orca-cli`) to hand
independent tasks to other agents in their own worktrees.

Delegated so far, both successful after a human re-check: the address-space spike and the DXVK/MoltenVK build (Codex).
Good next candidates (self-contained, clear done-criteria, no shared files): an x86 emulator reference for the lifter
tests; an in-game input/audio test checklist; the two Metal-oriented D3D9 DXVK options (`deAliasedSamplers`,
`renderTargetHazardCopies`) as a DXVK patch; a research task on a PhysX 2.8 adapter surface.
Keep with the main agent: `rt.h`, the lifter core, the build script, anything several tasks touch.

```text
orca worktree create --name <task> --no-parent --base-branch prasys/stingaree --agent codex --prompt "<brief>" --json
```

Lessons:
- Briefs must be self-contained: goal, done criteria, files to read, the absolute `work/` path, what not to touch,
  "do not commit". New worktrees branch from committed history, so uncommitted files are not visible to them.
- `--agent opencode` failed to launch three times ("could not confirm the launch outcome", no worktree created); `codex`
  worked. If a launch reports `retrySafe: true`, check `git worktree list` before retrying.
- A launch can report `prompt.outcome: "not-delivered"`: read the terminal (`orca terminal read --screen`); Codex may be
  sitting at an "Update available" prompt (choose Skip, do not run the installer), then send the brief with
  `orca terminal send --text "<brief>" --enter`.
- An agent saying "done" is not done: re-run its program and read its README. (The DXVK agent's summary said "done"
  while its own README listed an unfinished test.)
- Copy returned files into the main tree yourself (the agents do not commit); review before merging.
- Read-only discovery agents (`Agent` tool, general-purpose) are good for prior-art searches; treat their claims as
  leads and record which were read from source and which from READMEs.
