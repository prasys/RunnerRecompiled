# CLAUDE.md: index

Start here, then open the doc you need. Shared rules and lessons for all agents are in `AGENTS.md`; do not duplicate
them here.

## Directory

| Need | Go to |
|---|---|
| Repo rules, quick start, local `work/` layout, lessons learned, Orca delegation | `AGENTS.md` |
| macOS build from a clean Mac (user-facing guide: requirements, game extraction, CRT, rebasing, catalogs, DXVK, run) | `pc/MACOS-BUILD.md` |
| macOS port: phases, results per phase, risks, prior art, decisions | `pc/MACOS-PORT-PLAN.md` |
| Why the PC path was chosen (Russian, with numbers marked as assumptions) | `PC-PORT-ESTIMATE.md` |
| Design of the x86-32 lifter and its stages | `pc/LIFTER-DESIGN.md` |
| Project decisions (physics, saves, controls, device) | `DECISIONS.md` |
| Progress log of the Xbox/Android port | `CHECKLIST.md` |
| Memory spike: 16 KB pages, SIGBUS, zeroing | `pc/spikes/macos-addrspace/README.md` |
| DXVK on MoltenVK: build, patch, MoltenVK gaps, test program | `pc/spikes/macos-dxvk/README.md`, `pc/dxvk/dxvk-3.1.1-macos.patch` |
| macOS launcher (SwiftUI, settings -> env/`TdEngine.ini`, issue #6) | `pc/launcher/macos/MELauncher.swift`, `build.sh`, `make-icon.swift` (app icon); `pc/MACOS-BUILD.md` -> "Launcher" |
| One-app installer and DMG (issues #8, #15): wizard `SetupView.swift`, engine `Installer.swift`, `make-dmg.sh`, `bundle_dylibs.py`, CI `.github/workflows/macos-dmg.yml` | `docs/todo/INSTALLER.md` |
| Next-to-do items with their own notes (`MARGINS.md`: lost device, fixed; open questions remain) | `docs/todo/` |
| Upscaling spikes (MetalFX runs experimentally, FSR 1.0 not implemented; issue #4) | `pc/spikes/macos-metalfx/README.md`, `pc/spikes/macos-fsr/README.md`, `pc/dxvk/dxvk-3.1.1-macos.patch` (MetalFX folded in 2026-11-08); `AGENTS.md` -> "MetalFX / FSR upscaling" |
| DXVK on Android (the other host) | `pc/dxvk/README.md` |
| PhysX effects on Shatter (vendored code, port layer, changes to upstream, measurements) | `pc/shatter/UPSTREAM.md` |

| Code | Where |
|---|---|
| Host contract (`host_*`) | `pc/runtime/rt.h`; implementations `rt_host_win.c`, `rt_host_android.c`, `rt_host_macos.c` |
| Win32 emulation, VM, threads, EH | `pc/runtime/rt_core.c`, `rt_kernel32.c`, `rt_sync.c`, `rt_files.c`, `rt_user32.c` |
| D3D9 bridge (generated + hand-written + frame counter) | `pc/runtime/rt_d3d9_gen.c` (generated), `rt_d3d9.c`, `pc/tools/gen_d3d9_bridge.py` |
| Lifter and catalog | `pc/tools/{catalog,lift,gen_module,x86cfg,pe32,eh}.py` |
| Build | `pc/tools/build_pc_runtime.py` (`--macos`, `--android`, `--toolchain`) |
| Tests | `pc/tools/test_x86cfg.py`; `pc/harness/` (Windows only) |

## Where things stand (one line)

macOS port: the lifted game builds and runs on an M1 Max; title, menu and the tutorial level render and play at 60 fps
through DXVK on MoltenVK (text/colours, mouse, vsync fixed in session 2). Session 3: the upstream merge (EFLAGS/CRT
trig fix) removed the wrong-camera flashes, clipping and pose problems (user-observed); deferred destroy in the DXVK
patch fixed the lost GPU device on level loads. Session 4: PhysX fluids and cloth run on Shatter's native solvers (issue #2). **Issue #1 (level-start stall): pipeline warm-up added in session 5 (recorded states replayed on the compiler threads, see plan item 4); a cold-cache run still stalled for minutes, cause open.**
See the status table in `AGENTS.md` and the plan's "Phase 5 findings".

## Release v0.1.0 (2026-11-09): installer and DMG

First public release: `MirrorsEdgeForMac-v0.1.0.dmg` (80 MB) on the fork's Releases page. The launcher is also the
installer (wizard `SetupView.swift`, engine `Installer.swift`; headless `MELauncher --install <setup.exe> [--base dir]`);
everything lands in `~/MirrorsEdge` and is built on the user's Mac from their GOG copy (Apple clang, stock Python 3.9,
bundled libLLVM/SDL3/MoltenVK/Vulkan loader/DXVK/innoextract/cabextract/msitools). CI: `.github/workflows/macos-dmg.yml`,
runs only by hand or on a `v*` tag (push a tag only for a stable version). **Still to check:** the first run on a Mac
without Homebrew (user's M1 MacBook Air), then fix what breaks for v0.1.1; notarisation (without it: right-click > Open).
Details, layout and open items: `docs/todo/INSTALLER.md`.

## Next session starts here

00. **Session 7 start here:** cold level start = 47-55 s freeze; recorded tour + launch-time rebuild brings `escape_p` to 8.1 s, 187 states still compile
   although their shader pairs are recorded: diff those states against the warm file (plan, "Phase 5, session 7"). Then tour the other 8 maps
   (`pc/tools/gen_checkpoint_bindings.py`), try `start` from the title menu, make the launcher use the combined DXVK build, and find the ~1.7 s frame that is in every run.
0. **Warm-up recording (session 5):** finish recording the chapters by hand (`work/record-warm.sh`, checklist in
   `docs/todo/WARMUP-CHECKLIST.md`), then diagnose the long present waits (19-30 s with ~1,000 states; plan item 4,
   "Recording status and open problems") and the returning lost device. Ship the warm file only after both.
1. **Level-start stall** (GitHub issue prasys/RunnerRecompiled#1; plan, "Still open", item 4):
   profile the 23 s first frame, then precompile pipelines. Lost device (item 1) and flashes (item 2) are fixed
   (plan, "Phase 5, session 3"). Optional: the margin sweep in `docs/todo/MARGINS.md`. Read `AGENTS.md` -> "Lessons
   learned" first.
2. **DXVK build:** `pc/dxvk/build-macos.sh` builds a fresh tree from `pc/dxvk/dxvk-3.1.1-macos.patch` (all fixes incl. the map-tagged pipeline warm-up; output `work/dxvk/bin/`; after editing a DXVK tree regenerate the whole patch and check it applies to a clean v3.1.1). Older trees: all fixes (session 2, pool lock, deferred destroy) are in the patch; built
   tree `work/dxvk-defer/dxvk-3.1.1-git`, dylib copy `work/dxvk-defer/bin/libdxvk_d3d9.0.dylib`. `work/dxvk-textfix`
   and `work/dxvk-mac` are older.
   Runtime build with all fixes and the upstream merge: `work/pc-recomp-upstream` (`--work work/pc-recomp-upstream`);
   `work/pc-recomp-textfix` is the pre-merge baseline. Bounded unattended run: `work/run-bounded.sh <exe> <outdir> <secs>`.
3. **Unattended tutorial-load run** (no user needed; was the lost-device repro): `ME_PC_AUTO_KEYS=32,36,40,44,48 ME_PC_ASSUME_FOCUS=1
   ME_PC_MOUSE_CAPTURE=0 MVK_CONFIG_LOG_LEVEL=2`; it starts a new game and loads the tutorial; check the log for
   `mvk-error`. Add `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog` for Metal validation.
4. Frame dumps (`ME_PC_DUMP_FRAMES=...`, ranges allowed) are game art: keep them in `work/`. Convert PPM to PNG to view.
5. Merging upstream: it is CRLF, this repo LF (`.gitattributes`): `git merge -X renormalize origin/main`.
6. Unpushed work may exist: check `git status` and `git log fork/macos-port..HEAD` (branch `prasys/text-colour-debug`).

8. **Upscaling (issue #4):** MetalFX works in a bounded run through the launcher (patch + spike are in the repo; the dylib is
   built in `work/dxvk-metalfx/build-spike`, not committed). Open: a matched-crop image comparison in a game scene, a long-play
   and resize stability run, the 64 MiB buffer owner, then fold the patch into `dxvk-3.1.1-macos.patch`. FSR 1.0: implement
   EASU/RCAS in `DxvkSwapchainBlitter` (the Codex worker twice stopped after reading; use a stronger agent). Then the
   launcher's Output / internal-scale model (`AGENTS.md`, plan in the issue #4 discussion).

## Claude-specific notes

- This runs inside Orca. For independent tasks, delegate to Codex worktrees with the `orca-cli` skill rather than doing
  everything serially, and verify what comes back (`AGENTS.md`, "Working with Orca").
- Do not commit, push, or open PRs unless asked. Commit trailer when asked:
  `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Push target for macOS work: `fork` remote, branch `macos-port`.
- Report honestly: say what was measured and what was only assumed; the repo's docs mark unmeasured numbers
  **[допущение]**.
- When a phase changes state, update the table in `pc/MACOS-PORT-PLAN.md` and the status table in `AGENTS.md`; keep this
  index in sync when files are added or moved.
- Long-running builds and game runs go in the background (`run_in_background`, or `&` plus a bounded wait); there is
  no `timeout` on macOS.
