# CLAUDE.md

Read `AGENTS.md` first: project layout, hard rules (never commit game data), local `work/` layout, tools, and how to
delegate to other agents through Orca. The macOS plan is in `pc/MACOS-PORT-PLAN.md`.

Claude-specific notes:
- This runs inside Orca. For independent tasks (macOS spike, DXVK build), prefer delegating to Codex/OpenCode worktrees
  with the `orca-cli` skill rather than doing everything serially. Review their output before using it.
- Do not commit, push, or open PRs unless asked. Commit trailer when asked: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Report results honestly: say what was measured and what was only assumed; mark guesses as such (the repo's docs use
  **[допущение]** for unmeasured numbers).
- Update `CHECKLIST.md` and `pc/MACOS-PORT-PLAN.md` when a phase changes state.
