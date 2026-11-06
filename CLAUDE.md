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

macOS port: the lifted game builds and runs on an M1 Max; the main menu shows in the window at ~60 fps through DXVK on
MoltenVK, and keyboard/mouse work. **Known wrong: text is drawn as solid rectangles, colours/lighting are off.**
Unexplained: two SIGKILLs (exit 137) at ~60-70 s. See the status table in `AGENTS.md` and the plan's "Phase 5 findings".

## Next session starts here

1. Read `pc/MACOS-PORT-PLAN.md` -> "Phase 5 findings" (ranked to-do list) and `AGENTS.md` -> "Lessons learned".
2. Build and run with the quick start in `AGENTS.md`; add `ME_PC_DUMP_FRAMES=900,1500 ME_PC_DUMP_DIR=$PWD/work/frames`
   to get PPM dumps of the game's own render (convert to PNG to look at them; they are game art, keep them in `work/`).
3. First task: why text is a solid box (texture format / swizzle / sampler aliasing on MoltenVK). Then colours.
4. Delegation candidates (Orca/Codex): the two Metal-oriented DXVK D3D9 options as a patch; an investigation of the
   exit-137 kills.
5. Unpushed work may exist: check `git status` and `git log fork/macos-port..HEAD`.

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
