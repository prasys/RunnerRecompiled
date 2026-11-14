# "Prepare shaders" in the launcher/installer (design, issues #9-#11)

Status: design only (2026-11-15). No Swift or DXVK code is written. It depends on two tasks in flight:
**A** `pc/tools/warm_manifest.py` (manifest + the user's shader cache -> warm files) and **B** timing of the launch-time
rebuild on first and later launches. Numbers below are from the M1 Max, one run each, unless marked **[assumed]**.

## Goal and the one decision that blocks shipping

Ship a **manifest** (shader content hashes, pipeline states, map names; no bytecode) and rebuild
`pipelines.warm` + `pipelines.warm.shaders` on the user's Mac from their own `RefShaderCache-PC-D3D-SM3.upk`, so a level's
first visit has no compile freeze (`escape_p`: 56.5 s and 486 on-demand compiles -> 0 compiles when the recording was
replayed; one run, plan "Phase 5, session 8").

**Owner decision needed (for `DECISIONS.md`; not decided here): does the manifest count as "derived from game data"?**
`AGENTS.md` forbids committing "game data or anything derived from it".

- It is derived in the plain sense: it exists only because the game was played. It lists 3,161 hashes of the game's
  shaders, which of them pair up, on which map, and 751 raw render states (blend, depth, vertex layout, target formats)
  that DXVK built from the game's Direct3D calls.
- It holds no game bytes and cannot rebuild any asset: a hash only identifies a shader the user already has. The repo
  already commits facts of this kind (`gameExeSHA256`, the CRT hashes and the DLL rebase table in `Installer.swift`;
  map names in the docs).
- Options: (1) commit the manifest; (2) keep it out of git and add it only as a CI/release input to the DMG (still
  distributed, the same question for the release); (3) ship nothing and let each user record by playing (today).
  Everything below works for (1) and (2). With (3) only the folder move in part 2 is worth doing.

## 1. Where it runs and what the user sees

Two stages with very different costs:

| Stage | What | Cost | Needs |
|---|---|---|---|
| **a. Build warm files** | `warm_manifest.py`: scan the upk, match hashes, write the two files | Seconds: the upk scan (45,437 shaders) took 1.6 s with `/usr/bin/python3` 3.9.6, hashing them all 0.05 s (measured here); the rest is task A's | The extracted game only |
| **b. Compile** | DXVK's launch-time rebuild builds every shader and state | 77-86 s cold for 3,157 shaders / 3,464 states (session 8). Later launches: task B | The built `me_pc`, a GPU device, a window |

- **Install wizard:** a new step "Preparing shaders" after "Building the macOS game" runs stage a. It is never fatal:
  any failure is logged to `logs/shaders.log` and the install continues. The "Ready to play" text then says what
  happened ("Shaders prepared for 11 chapters" or "Shaders could not be prepared; levels will pause while they load the
  first time").
- **Stage b, phase 1 (no new code path):** it already happens at the title menu on every launch. The wizard's done
  screen and the launcher status line say: "On the first start, wait in the menu for about a minute and a half." This
  is the configuration that was measured.
- **Stage b, phase 2 (prepare run):** the launcher starts `me_pc` in a prepare mode that runs the rebuild at full speed
  with nothing else going on, reports `n/total`, and exits. The launcher shows a sheet: progress bar from the counts,
  elapsed time, "about 1-3 minutes" **[assumed for slower Macs; 77-86 s measured on one M1 Max]**, and Cancel (kills the
  process; what was compiled stays in DXVK's cache **[assumed]**; the step can be re-run). It runs at the end of the
  install and from a button. **Build phase 2 only if task B shows a later launch's rebuild is much shorter than the
  cold one.** If it is not, an up-front run buys nothing and the work should go into the menu wait instead (open
  question 1).
- **Launcher (Game tab):** next to the existing controls: a status line ("Shaders: 3,490 of 3,490 pipelines prepared,
  2026-11-15" / "not prepared" / "out of date: app updated"), a **Prepare shaders** button (re-run; disabled while the
  game runs), and the existing **Clear warm-up data (cold start)**. Clear deletes the warm files and the stamp, so the
  status becomes "not prepared" and its message changes to "…Press Prepare shaders to rebuild them from your game
  files." Clear stays a real cold start for testing; it must not trigger an automatic re-prepare.
- **Headless:** `MELauncher --prepare-shaders [--base dir]` for tests and CI-style checks.

## 2. Which files go where

Today (read from the code): the launcher sets neither path. The patch writes `$HOME/.cache/dxvk/pipelines.warm` and
`.shaders` unless `DXVK_PIPELINE_WARM_FILE` is set. DXVK's own cache (`<hash>.dxvk.bin/.lut`) goes to
`DXVK_SHADER_CACHE_PATH`, else `$XDG_CACHE_HOME/dxvk`, else `~/.cache/dxvk`; its file name is a hash of the last two path
parts of the executable (`bin-macos/me_pc`), so it does not change with the install folder. **DXVK 3.1.1 has no
`DXVK_STATE_CACHE_PATH`**; the variable is `DXVK_SHADER_CACHE_PATH`. `~/MirrorsEdge/cache/` is created by the installer
and unused. So game bytecode currently sits outside the folder the wizard promises holds everything.

Proposed, for installed builds (a new `cacheDir` setting, filled by `stepFinish`; empty = today's behaviour for dev trees):

```
~/MirrorsEdge/cache/dxvk/
  pipelines.warm, pipelines.warm.shaders   DXVK_PIPELINE_WARM_FILE=<dir>/pipelines.warm
  <hash>.dxvk.bin, .dxvk.lut               DXVK_SHADER_CACHE_PATH=<dir>
  prepare.json                             stamp: manifest id, DXVK build id, upk size+mtime, found/missing counts,
                                           stage b result, date
<app>/Contents/Resources/warm/manifest.bin (+ manifest.json header)     read-only, shipped
```

- **Merge, never overwrite.** DXVK appends the user's own records to the same `pipelines.warm` while they play.
  Stage a must keep every existing record and add the manifest's missing ones (write to a temp file, rename; refuse
  while the game runs). Task A's tool needs a `--merge-into` mode or the launcher must not call it on an existing file.
- **Names only have to agree between the two files.** When a created shader's own name differs from the recorded one,
  the patch registers it under the recorded name too (`warmShaderRegistered(made, e.name)`), so the tool does not have
  to reproduce DXVK's shader hash.
- **Migration (v0.1.0 users):** on first start with `cacheDir` set and empty, move `~/.cache/dxvk/pipelines.warm*` and
  the `.dxvk.*` files in. `clearWarmData` clears the new folder and still the old one.

## 3. Finding the upk; other game builds

- Path: `<gameDir>/../TdGame/CookedPC/RefShaderCache-PC-D3D-SM3.upk` (`gameDir` is `…/pc/Binaries`); look the name up
  case-insensitively. Fallback: a "Locate…" file dialog is not worth it for the installer build (the layout is ours).
- The installer only accepts the GOG 1.01 executable (SHA-256 check), so every installed build has the upk the
  recording came from. Steam/EA/retail copies only reach this through a manual build (`pc/MACOS-BUILD.md`). Whether
  their shader cache is byte-identical is **unknown** (not checked; we have one copy).
- Rule for any build: match by content hash, per shader. A pipeline record is written only if both of its shaders were
  found; the rest are dropped and counted. DXVK then compiles those on demand as it does today. The step reports
  "found N of M shaders" and succeeds with any N, including 0 (then it writes nothing and says so).
- Do not gate on a hash of the whole upk (121 MB); the per-shader match is the check. The stamp keeps size+mtime only
  to notice a changed file.

## 4. How the manifest ships

- In the DMG at `Resources/warm/`, copied by `make-dmg.sh`; under option (2) above CI fetches it from a release asset
  instead of the repo. Size: the 3,490 records de-duplicated (751 states x 416 bytes, 3,161 names, 3,490 index rows) are
  451 KB raw and 83 KB with zlib (measured here with a throwaway layout; task A's format decides the real number).
- Header fields the launcher and the tool check: manifest format version; warm file version (`WarmVersion`, now 2);
  `sizeof(DxvkGraphicsPipelineStateInfo)` (416); DXVK base version (`3.1.1`) and the SHA-256 of
  `pc/dxvk/dxvk-3.1.1-macos.patch` it was recorded with; game build (the exe SHA-256); counts per map; recording date.
- Why so strict: the state is a raw struct. A different size makes the patch rename the file to `.old` **and delete
  `.shaders`**; the same size with a different meaning would replay wrong states silently. So: the tool refuses a
  manifest whose state size or DXVK id differs from the bundled build's, and CI fails the DMG build on a mismatch. A
  patch change that does not touch the state layout still changes the patch hash, so the header needs an explicit
  "state layout id" bumped by hand, with the patch hash kept as information only (open question 4).
- The states also depend on game settings **[assumed, not tested]**: the recording was made with one anti-aliasing
  setting; a different MSAA level gives different states, which compile on demand.

## 5. Failure modes and what the launcher says

| Failure | Behaviour | Message |
|---|---|---|
| upk missing or unreadable | skip, stamp `no-upk` | "The game's shader file was not found; levels will pause the first time." |
| Manifest missing, or built for another DXVK build | skip, stamp `manifest-mismatch` | "Shader list does not match this app version." (a packaging bug; log it) |
| Partial match (other game build) | write what matched | "Prepared 2,900 of 3,490 pipelines." |
| Tool crash, disk full | keep the old files (temp + rename) | "Preparing shaders failed; see logs/shaders.log." Play stays enabled |
| Game running | refuse | "Close the game first." |
| Prepare run (phase 2): device lost, hang, crash | The launcher watches the output for `DEVICE_LOST` / `mvk-error`, a non-zero exit, and no progress for 60 s; it kills the run and stamps `compile: partial n/total`. One automatic retry, then stop | "Shader compilation stopped early (n of total). You can play; run Prepare shaders again later." |
| Device loss in play (issue #13) | Not this step's to fix. The warm files are append-only and survive it | none |

Device loss and the rebuild: of the two replay runs in session 8, the one where a level started while the rebuild was
still running lost the device, the other did not; the three losses during recording were not near a compile. Whether
a rebuild with no level loaded can lose the device is **untested**. The prepare run is the cleanest way to test it.

## 6. Later, separate phase: opt-in "share new pipelines"

Not part of this work. A Game-tab button "Export new pipelines…" writes one small file: the records in the user's
`pipelines.warm` that the shipped manifest lacks, as hashes + states + map names (the tool hashes the bytecode from
`.shaders`; no bytecode, paths or machine data in the file). The user attaches it to a GitHub issue by hand; the
launcher does no network I/O. A maintainer merges it with a `warm_manifest.py --merge` mode after checking every hash
exists in the reference upk. It falls under the same owner decision as the manifest, and only GOG 1.01 files should be
accepted until other builds are understood.

## 7. Implementation plan (effort figures are estimates)

Phase 1: stage a, folder move, UI (about 1.5-2 days **[estimate]**)

1. `MELauncher.swift`: `Settings.cacheDir`; `environment(for:)` sets `DXVK_PIPELINE_WARM_FILE` and
   `DXVK_SHADER_CACHE_PATH` when it is set; `clearWarmData` takes the folder (plus the legacy one) and removes
   `prepare.json`; Game tab status line and **Prepare shaders** button; `--prepare-shaders`. (0.5 day)
2. New `PrepareShaders.swift`: locate upk and manifest, run `/usr/bin/python3 -I …/pc/tools/warm_manifest.py` through
   the existing `Installer.run` (log to `logs/shaders.log`, parse a final `found N of M` line), read/write
   `prepare.json`, one-time migration from `~/.cache/dxvk`. (0.5 day)
3. `Installer.swift`: step `shaders` after `build`, wrapped so it cannot fail the install; `stepFinish` fills
   `cacheDir`. `SetupView.swift`: the step shows through `installSteps`; new "Ready to play" text. (0.25 day)
4. `make-dmg.sh`: copy the manifest to `Resources/warm/`, check its header against the built dylib, and run the tool
   under `env -i /usr/bin/python3 -I` like the existing smoke test. `build.sh` for dev trees. (0.25 day)
5. Docs: `pc/MACOS-BUILD.md` "Pipeline warm-up", `docs/todo/INSTALLER.md` layout, `AGENTS.md` env table. (0.25 day)
6. Test: fresh `--install --base <scratch>`, then a cold `escape_p` run as in session 8 against the *rebuilt* files (the
   measured run used the recorded files directly); then the other maps.

Phase 2: prepare run (about 2-3 days **[estimate]**, only after task B's answer)

7. DXVK patch (`dxvk_pipemanager.cpp/.h`, `d3d9.macos.exports`): a progress log line every ~100 states and a final
   line once the queue is really empty (today's "boot rebuild done" is printed when the last states are *queued*); export
   `dxvk_warm_boot_status(done, total)`; `DXVK_PIPELINE_WARM_PREPARE=1` starts the rebuild at device creation, at normal
   priority and without the "< 6 in flight" throttle. Regenerate the whole patch and check it applies to a clean v3.1.1.
8. `rt_host_macos.c`: `ME_PC_PREPARE=1` polls that status, prints progress, and exits 0 when it is complete. It must be
   the same `me_pc` binary: DXVK's cache file name comes from the executable path, and Metal's own cache is likely
   keyed per executable too **[assumed]**, so a separate small helper program would warm the wrong caches.
9. `PrepareShaders.swift`: run it with a pipe, the progress sheet, Cancel, the watchdog and retry of part 5.

## Open questions

1. **Task B:** how long is the rebuild on a second launch (DXVK cache warm, Metal cache warm)? This decides phase 2 and
   whether the menu needs a visible "preparing" hint or the rebuild should start with the map in the last save.
2. **Owner decision** on the manifest (top of this document), and where it lives: repo or release asset.
3. The measured 0-compile result used the recorded files. Do the files rebuilt from manifest + upk give the same result?
   (Should, if byte-equal shaders and states; step 6 checks it.)
4. Who bumps the "state layout id", and can CI detect a layout change (for example a `static_assert` on the size in the
   patch plus a constant the manifest header must equal)?
5. Can the prepare run keep its window hidden or tiny? `rt_host_macos.c` already creates hidden secondary windows, but
   presenting to a hidden main window on MoltenVK is untested.
6. Does anything in the state depend on the Mac (GPU family, MoltenVK version) or on game settings (MSAA)? Only an
   M1 Max with one settings profile has been recorded.
7. Coverage: one play-through by one player. Other routes, difficulty-dependent scenes and the time trials are not in
   it; part 6 is the path to grow it.
8. Does a rebuild that runs while a level loads trigger the device loss (issue #13)? If yes, phase 2 matters more.
