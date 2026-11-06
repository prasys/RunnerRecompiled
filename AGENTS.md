# AGENTS.md

Shared context for coding agents (Claude Code, Codex, OpenCode, and others) working in this repo.

## What this repo is

RunnerRecompiled: a fan-made static recompilation of *Mirror's Edge* (2008).

- `xbox/`, `android/`: Xbox 360 build, PowerPC lifted with XenonRecomp, own kernel/XAM runtime, patched RexGlue Vulkan
  GPU backend, Android ARM64 app.
- `pc/`: **the Windows PC build**, x86-32 lifted to C by our own lifter, with its own Win32 runtime and a Direct3D 9
  bridge to DXVK Native. Runs on Windows x64 (bring-up) and Android ARM64.
- Current effort: **macOS (Apple Silicon) host for the PC build.** Plan: `pc/MACOS-PORT-PLAN.md`. Read it first.

Most older notes are in Russian. Public docs and new notes are English.

## Hard rules

- **Never commit game data or anything derived from it.** `work/`, `private/`, `build/`, `*.xex`, `*.iso`, `*.bin`,
  `*.tfc` are gitignored. Lifted C, catalogs, rebased DLLs, extracted installers stay in `work/`.
- The lifter, runtime and scripts are original code (MIT). Do not copy code from other recomp projects; credit ideas.
- Do not run or modify the user's Downloads (`*.iso`, `*.bin`, installers). Read-only.
- Do not touch the Xbox/Android code paths unless the task says so.
- Do not commit or push unless asked.

## Local layout (this Mac, set up 2026-11-06)

The `work/` folder lives only in the main checkout:
`/Users/pradeesh/orca/workspaces/RunnerRecompiled/stingaree/work/`. Other worktrees do not have it; use that absolute
path (read-only for inputs).

| Path | Contents |
|---|---|
| `work/pc/` | GOG install extracted with `innoextract` (from `~/Downloads/setup_mirrors_edge_1.01_jpfix_(41238).exe`). Game in `work/pc/Binaries`, data in `work/pc/TdGame`. |
| `work/pc-deps/` | `msvcr80.dll`, `msvcp80.dll` (from the VC 2005 redistributable) |
| `work/pc-deps/rebased/` | the 11 DLLs rebased by `pc/tools/rebase_pe.py`: PhysXLoader 0x20000000, PhysXCore 0x21, (PhysXCooking 0x22, missing), NxCooking 0x23, PhysXExtensions 0x24, NxCharacter 0x25, ogg 0x26, vorbis 0x27, vorbisfile 0x28, vorbisenc 0x29, libresample 0x2a, binkw32 0x2b (all `xx000000`) |
| `work/x/` | scratch unpacking of the redistributables (can be deleted) |
| `work/pc-recomp/` | (not created yet) catalogs, generated C, build output |

Facts to rely on:
- `MirrorsEdge.exe` SHA-256 `58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd`. Catalog addresses are
  tied to this exact build. Expected catalog: 178,605 functions, 5.73 M instructions, 10,303 switch-table entries.
- The GOG install renames `PhysXCore.dll` to `PhysXCore.bak` (it is the real 2.8.0 FC7 DLL). `work/pc/Binaries` has a
  copy named `PhysXCore.dll`.
- The game ships its own `NxCooking.dll`, `PhysXLoader.dll`, `PhysXExtensions.dll`, `NxCharacter.dll`. They differ
  from the copies in the 9.17 PhysX System Software; use the game's.
- PhysX 2.8.0 has no `PhysXCooking.dll`; see "Open questions" in the plan.

## Tools installed (Homebrew, arm64, macOS 15.8)

`llvm@23`, `llvm`, `molten-vk`, `vulkan-headers`, `vulkan-loader`, `glslang`, `spirv-cross`, `sdl3`, `sdl2`, `ninja`,
`meson`, `openal-soft` (keg-only; macOS also ships the deprecated OpenAL.framework), `innoextract`, `sevenzip` (`7zz`),
`cabextract`, `msitools`. No Wine.

Python scripts are run isolated: `python3 -I pc/tools/<script>.py ...`. The older docs show
`outputs/mirrors-edge-native/pc/tools/...` paths from a different tree; here the path is `pc/tools/...`.

## Build and test notes

- The lifter (`pc/tools/catalog.py`, `lift.py`) disassembles through LLVM MC via ctypes. Docs refer to
  `libLLVM-23.dll` from llvm-mingw; on macOS use `/opt/homebrew/opt/llvm@23/lib/libLLVM.dylib`.
- `pc/tools/build_pc_runtime.py` currently targets a Windows x64 host with llvm-mingw. It needs a macOS mode
  (native arm64 clang, same CFLAGS: `-O2 -g0 -fwrapv -ffp-contract=off -fno-strict-aliasing`).
- Tests: `pc/tools/test_x86cfg.py` (17 cases, pure byte decoding, passes on macOS). `pc/harness/` (differential test) runs
  the original x86-32 code natively in a 32-bit Windows process, so it **cannot run on this Mac**.
- Catalog check on macOS: the tools at commit `4f2d67e` reproduce 178,605 reachable functions; current tools give 185,536.
- macOS build: `python3 -I pc/tools/build_pc_runtime.py --macos /opt/homebrew/opt/llvm@23 --work work/pc-recomp` (from the
  project root; stops before linking until `pc/runtime/rt_host_macos.c` exists).
- macOS address-space facts (host page 16 KB, SIGBUS faults, no zeroing on MADV_DONTNEED): `pc/spikes/macos-addrspace/README.md`.
- Guest memory: one 4 GB reservation, guest address = offset from base (`rt_core.c`, `host_reserve`).

## Working with Orca (agent delegation)

This repo is worked on inside Orca. Use the `orca` CLI (skill `orca-cli`; `orca skills get orca-cli`) to hand
independent tasks to other agents (`codex`, `opencode`, `claude`) in their own worktrees.

Good tasks to delegate (self-contained, clear done-criteria, no shared files):
- Phase 2 spike (standalone C test program).
- Phase 4 DXVK build for macOS and the D3D9 triangle test.
- Phase 3 pieces after the spike: `rt_host_macos.c` memory/threads, SDL3 input/window.

Keep with the main agent: edits to `rt.h`, the lifter core, the build script, anything several tasks would touch.

Pattern:

```text
orca worktree create --name <task> --no-parent --base-branch prasys/stingaree --agent codex --prompt "<brief>" --json
```

Briefs must be self-contained: goal, done criteria, files to read (`AGENTS.md`, `pc/MACOS-PORT-PLAN.md`), the absolute
`work/` path, what not to touch, and "report results in `pc/notes/<task>.md`, do not commit". New worktrees branch from
committed history only, so uncommitted files in the main checkout are not visible to them; point to absolute paths.
Check progress with `orca terminal read` and `orca worktree ps`. Review every returned change before merging.
