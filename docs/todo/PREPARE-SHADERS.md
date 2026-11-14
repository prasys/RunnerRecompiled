# "Prepare shaders" in the launcher: local recording only (design, issue #11)

Status: design only (2026-11-15, revised after the owner's decision). No Swift, runtime or DXVK code is written.
Numbers are from the M1 Max, one run each, unless marked **[estimate]** or **[assumed]**.

## Decision this design follows

`DECISIONS.md` (2026-11-15, commit `bb911e1`): everything obtained from game data is derived and is not distributed. That
covers the recorded `pipelines.warm` / `.shaders`, the shader cache, logs holding such data, and also a manifest of
shader hashes + pipeline states + map names. **No manifest ships**, in the repo, the DMG or a release asset. Why it was
on the table: it holds no game bytes and would have given every user the measured result at once (`escape_p`: 56.5 s
freeze and 486 on-demand compiles -> 0 compiles). Why it is out: it exists only because the game was played and lists
which shaders the game uses, per map. Only scripts, the checkpoint list, the file format and analysis tools may ship.
`pc/tools/warm_manifest.py` (branch `prasys/warm-manifest`) is not part of this design; it stays a local tool (backup,
moving a recording between one's own Macs).

Consequence: every user records on their own Mac. A level's first visit is cold unless something visited it before.
The two parts below are (a) making the recording that already happens visible, and (b) an optional tour that visits
the game for the user.

## (a) Passive recording, made visible

What already happens (no change): while the user plays, the patched DXVK appends each new pipeline to `pipelines.warm`
and its two shaders' bytecode to `pipelines.warm.shaders`, tagged with the map. On the next launch the launch-time
rebuild compiles all of it at the title menu (77-86 s for a full play-through's 3,490 pipelines on a cold DXVK cache;
later launches: task B measures). A chapter played once does not freeze on the next visit.

Launcher, Game tab, new "Shaders" box next to the existing warm-up controls:

- **Per-map table**, read straight from `pipelines.warm` (header `MEPW`, version, state size; then the records; about
  40 lines of Swift, the same walk as `warm_stats.py`): chapter label, map, pipelines recorded. Plus totals, file sizes
  and the file's date. The chapter labels are a small fixed table of the ten story maps and the menu.
- **No "% complete" and no expected counts.** A per-map target ("Escape has 717") would be a number taken from our
  recording, which is derived data. The table shows what is recorded, and "not visited yet" for a map with no records.
- **State line:** "Recording on" / "off" (the existing toggle), and a warning when the file's header does not match the
  bundled DXVK (the patch would then rename it to `.old` and delete `.shaders` on the next start; say so before Play).
- **Clear warm-up data (cold start)** stays as it is, pointed at the new folder; the table empties.
- **Record automatically…** opens part (b).
- First-start text in the wizard's "Ready to play": "Levels pause while they load the first time you visit them. The
  game remembers; the next visit is smooth." No time promise.

## (b) Optional automated recording tour

What ships (none of it game data): `pc/tools/gen_checkpoint_bindings.py`, a checkpoint list `pc/tools/checkpoints.txt`
(181 lines `start <Map>?LoadCheckpoint=<name>` for 10 maps, taken from the LucasOe gist: credit it; its licence is an
open question), and the launcher code that drives the game. The list is not in the repo today (`work/record-all/`).

**One short game process per map.** Long sessions lost the GPU device three times during the by-hand recording
(Cranes ~22 min in, Convoy after ~8 loads, one replay run; issue #13) and restarting between chapters avoided it. So
for each of the 10 maps, in story order:

1. Copy the user's profile (`pc-user`) to `cache/tour/profile`, fresh each time. The user's real settings, bindings
   and saves are never touched.
2. Run the generator on the copy's `TdInput.ini` with `--maps <Map>`: key `H` loads the next checkpoint per press. It
   prints the `ME_PC_AUTO_INPUT` string (presses keyed to frame numbers, which do not advance during a compile stall).
3. Start `me_pc <game> <deps> <copy>` with `ME_PC_AUTO_KEYS=32,36,40,44,48` (new game -> tutorial; `start` has only
   been tested from inside a level, not from the title menu), the `ME_PC_AUTO_INPUT` string, `ME_PC_ASSUME_FOCUS=1`,
   `ME_PC_MOUSE_CAPTURE=0`, `ME_PC_LOG_FILES=1`, the real `DXVK_PIPELINE_WARM_FILE` (appending to it is the point) and
   `DXVK_PIPELINE_WARM_BOOT=0` (no launch-time rebuild in tour processes: it costs time in each one, and the one replay
   run where a level started during the rebuild lost the device).
4. Read the process output. A `warm-up map:` line and the frame counter tell which checkpoint is loaded; `Pipeline
   recorded #N` gives the live count.
5. End the process after the last checkpoint's dwell, then start the next map. Ending needs a small runtime addition:
   a `<frame>:quit` action for `ME_PC_AUTO_INPUT`. Until then the launcher kills the process.

**Time.** The one unattended tour so far: 23 checkpoints (Edge_p + Escape_p), about 15 min, 1,500 frames apart, 0
errors. That is ~39 s per checkpoint, so 181 checkpoints are roughly **2 hours [estimate, linear]**, plus ten process
starts with the tutorial load, about 1.5 min each **[estimate]**. Stormdrain alone has 37 checkpoints. The user can pick
chapters; the default order is story order, so stopping early still covers what they will play first.

**What the user sees.** A sheet: the ten maps with a state each (waiting, running n/m checkpoints, done, incomplete,
skipped), pipelines recorded so far, elapsed time, and a plain warning before the start: "This plays through the game's
checkpoints by itself for about two hours. The game window stays open and makes sound; you can use other apps. Your
saves are not touched." Whether the Mac is comfortably usable meanwhile is **[assumed]**.

**Cancel / resume.** Cancel kills the current process. Nothing is lost: the warm file is append-only and written per
pipeline. `cache/tour/tour.json` keeps, per map, the checkpoints reached and the result; "Continue" starts at the first
checkpoint not reached (the generator needs a `--skip N` option). Closing the launcher is the same as Cancel.

**Device loss or hang mid-tour.** The launcher treats any of these as a failed process: `DEVICE_LOST` or `mvk-error` in
the output, a non-zero exit, no new frame for 90 s. It kills the process (after a device loss the game does not
recover), keeps everything recorded, and restarts that map once from the next unreached checkpoint. A second failure
marks the map "incomplete" and the tour goes on. The summary lists incomplete maps; the game still works there, cold.
The tour never blocks Play and never reports a failure as success.

**Coverage is the open risk.** A checkpoint load shows only what is on screen at the spawn point, standing still.
Measured here against the by-hand play-through (3,490 pipelines, 3,161 shaders), comparing by shader bytecode and by
(vertex shader, pixel shader, state):

| By-hand recording, scope | Shaders | Tour has | Pipelines | Tour has |
|---|---|---|---|---|
| Edge_p + Escape_p (the two maps toured) | 1,064 | 546 (51%) | 1,072 | 485 (45%) |
| Whole game | 3,161 | 833 (26%) | 3,463 | 809 (23%) |

So on the maps it visited, that tour recorded about half of what playing them recorded. In a cold `escape_p` test its
file left 187 on-demand compiles and an 8.1 s first frame (control: 553 compiles, 47 s; by-hand file: 0 compiles). A
tour of this kind shortens the freezes; it does not remove them. Whether a better tour gets close is not known.
Also unexplained: the tour file tags only 8 pipelines `edge_p` although the log shows 20 Edge loads, and 311 as
`scraper_p` (the profile started there). Tags decide the per-map table and the per-map replay, so this needs a look.

## Where the files live

Today the launcher sets no cache path, so the warm files and DXVK's cache (`<hash>.dxvk.bin/.lut`; the variable is
`DXVK_SHADER_CACHE_PATH`, there is no `DXVK_STATE_CACHE_PATH` in 3.1.1) land in `~/.cache/dxvk`, outside the folder the
wizard says holds everything; `~/MirrorsEdge/cache/` exists and is unused. Proposed for installed builds (a new
`cacheDir` setting filled by `stepFinish`; empty keeps today's behaviour for dev trees):

```
~/MirrorsEdge/cache/dxvk/   pipelines.warm, pipelines.warm.shaders   (DXVK_PIPELINE_WARM_FILE)
                            <hash>.dxvk.bin, .dxvk.lut               (DXVK_SHADER_CACHE_PATH)
~/MirrorsEdge/cache/tour/   profile/ (copy), tour.json, logs/<map>.log
```

One-time move of an existing `~/.cache/dxvk` recording (v0.1.0 users) into the new folder; Clear covers both places.
Deleting `~/MirrorsEdge` then removes every game-derived byte.

## Measuring a tour against the by-hand recording

The reference is `work/record-maps/20261114-200435/` (local, never committed). All outputs stay in `work/`.

1. Extend `pc/tools/warm_stats.py` with `--compare <reference.warm>`: per map and overall, shaders and pipelines of the
   reference that the other file has, matched by bytecode and state (the table above came from a throwaway version;
   `--old-shaders` only gives one overall shader number).
2. Run the tour per map into an empty scratch warm file (`DXVK_PIPELINE_WARM_FILE`), one process per map as designed,
   and record per map: checkpoints loaded, wall time, device losses, coverage from step 1.
3. The measure that matters: a cold `escape_p` run (empty DXVK cache) replaying only the tour's file, as in session 8.
   Report on-demand compiles and the longest frame next to the control (486 compiles, 56.5 s) and the by-hand file (0).
   Then the same on two other maps.
4. Variants, one at a time, same measures: a camera turn at each checkpoint (`<frame>:mouse=<dx>,0,<frames>`, already
   supported), a few seconds of walking forward, shorter and longer dwell than 1,500 frames.
5. Decide with the numbers whether (b) is worth shipping. Suggested bar for the owner to set: how many compiles and how
   long a first-frame freeze are acceptable after a tour. If no variant gets near it, ship (a) alone.

## Implementation plan (effort figures are estimates)

Phase 1, part (a) and the folder (about 1 day):
1. `MELauncher.swift`: `Settings.cacheDir`; `environment(for:)` sets the two cache variables when it is set;
   `clearWarmData` takes the folder plus the legacy one; warm-file reader and the Shaders box; header-mismatch warning.
2. `Installer.swift` `stepFinish`: fill `cacheDir`, create `cache/dxvk`; one-time move from `~/.cache/dxvk`.
   `SetupView.swift`: the "Ready to play" text.
3. Docs: `pc/MACOS-BUILD.md` "Pipeline warm-up", `docs/todo/INSTALLER.md` layout, `AGENTS.md` env table.

Phase 2, measure before building (about 1-2 days, mostly unattended run time): the plan in the section above, with
shell scripts in `work/`, no launcher code. Needs `checkpoints.txt` and the generator fixes in step 5.

Phase 3, the tour in the launcher (about 3-4 days), only if phase 2 says it is worth it:
4. New `RecordTour.swift`: per-map loop, profile copy, process with a pipe (as `Installer.run`), output parsing,
   watchdog, retry, `tour.json`, the sheet; headless `MELauncher --record-tour [--maps …]`.
5. `gen_checkpoint_bindings.py`: `--skip N`; and accept `bEnableMouseSmoothing=` in either case and value. Today it
   exits unless the line is exactly `=true`, and the launcher's own mouse-smoothing setting writes `=False`.
6. `rt_host_macos.c`: the `<frame>:quit` action. No DXVK patch change is needed for any of this.
7. `pc/tools/checkpoints.txt` committed with credit; `make-dmg.sh` already copies `pc/tools`.

## Open questions

1. Coverage (above): can any automated tour come close to a play-through? Phase 2 answers it.
2. The checkpoint list comes from a third-party gist with no stated licence. Ship it, ask the author, or rebuild the
   list ourselves?
3. Does `start <Map>?LoadCheckpoint=` work from the title menu? If yes, the tutorial load per process goes away.
4. A fresh install has no `TdInput.ini` until the game has started once. Does the tour's first process create it, or
   must the user start the game once first?
5. Why are the old tour's map tags wrong for Edge_p?
6. Is per-map restart enough against device loss? Stormdrain (37 checkpoints, ~24 min **[estimate]**) is as long as the
   Cranes session that lost the device. A cap of ~15 checkpoints per process may be needed.
7. Can the tour run with the window small and the sound off? No switch exists for sound today.
8. Task B: how long the menu rebuild takes on later launches. If it stays long with a full recording, the Shaders box
   should show "compiling n/total" while the game is at the menu (needs a progress line from the patch).
