# CLAUDE.md: index

Start here, then open the doc you need. Shared rules and lessons for all agents are in `AGENTS.md`; do not duplicate
them here.

## Directory

| Need | Go to |
|---|---|
| Repo rules, quick start, local `work/` layout, lessons learned, Orca delegation | `AGENTS.md` |
| macOS port: phases, results per phase, risks, prior art, decisions | `pc/MACOS-PORT-PLAN.md` |
| Why the PC path was chosen (Russian, with numbers marked as assumptions) | `PC-PORT-ESTIMATE.md` |
| Design of the x86-32 lifter and its stages | `pc/LIFTER-DESIGN.md` |
| Project decisions (physics, saves, controls, device) | `DECISIONS.md` |
| Progress log of the Xbox/Android port | `CHECKLIST.md` |
| Memory spike: 16 KB pages, SIGBUS, zeroing | `pc/spikes/macos-addrspace/README.md` |
| DXVK on MoltenVK: build, patch, MoltenVK gaps, test program | `pc/spikes/macos-dxvk/README.md`, `pc/dxvk/dxvk-3.1.1-macos.patch` |
| Next-to-do items with their own notes (start with `MARGINS.md`) | `docs/todo/` |
| DXVK on Android (the other host) | `pc/dxvk/README.md` |

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
through DXVK on MoltenVK (text/colours, mouse, vsync fixed in session 2). **Known wrong: the GPU device is lost during
some level loads (game freezes), and single frames are drawn from the wrong camera while walking.** See the status
table in `AGENTS.md` and the plan's "Phase 5 findings".

## Next session starts here

1. **Next to do, first: `docs/todo/MARGINS.md`** (lost GPU device on level load). Root cause found: MoltenVK
   releases Metal objects on destroy while command buffers still need them. A deferred-destroy DXVK change with
   `DXVK_DEFER_MARGIN=200` gave 0 of 5 freezes (vs 4 of 6 without the margin). It is saved as
   `docs/todo/dxvk-defer-destroy-wip.patch` and is not yet in the main DXVK patch. Next: sweep the margin, then make it
   a proper fix. After that: `pc/MACOS-PORT-PLAN.md` -> "Still open", item 2 (one-frame wrong-camera flashes; an Opus
   worker investigated it in Orca worktree `flash-investigation`, so check its branch `prasys/flash-investigation`),
   then clipping (3) and shader precompile (4). Read `AGENTS.md` -> "Lessons learned" first.
2. **DXVK build:** the session-2 fixes are in `pc/dxvk/dxvk-3.1.1-macos.patch`; the built copy is
   `work/dxvk-textfix/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib`. `work/dxvk-mac` is older (no fixes).
   Runtime build with all fixes: `work/pc-recomp-textfix` (`--work work/pc-recomp-textfix`).
3. **Unattended repro of the freeze** (no user needed): `ME_PC_AUTO_KEYS=32,36,40,44,48 ME_PC_ASSUME_FOCUS=1
   ME_PC_MOUSE_CAPTURE=0 MVK_CONFIG_LOG_LEVEL=2`; it starts a new game and loads the tutorial; check the log for
   `mvk-error`. Add `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog` for Metal validation.
4. Frame dumps (`ME_PC_DUMP_FRAMES=...`, ranges allowed) are game art: keep them in `work/`. Convert PPM to PNG to view.
5. Unpushed work may exist: check `git status` and `git log fork/macos-port..HEAD` (branch `prasys/text-colour-debug`).

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
