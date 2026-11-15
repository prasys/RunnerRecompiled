# One-app installer and DMG (issues #8, #15)

Goal: the user downloads a DMG, drags the app to Applications, starts it, drops their legal GOG copy of *Mirror's Edge*
on it, and waits. The app does everything in `pc/MACOS-BUILD.md` steps 1-7 and then plays. Nothing derived from the game
is ever shipped; it is all built on the user's Mac.

## Decisions (2026-11-08)

- Toolchain: the user needs only Xcode Command Line Tools. The installer checks for them (`xcode-select -p`) and offers
  `xcode-select --install`.
- Input: the user drops the GOG `setup_mirrors_edge_1.01_jpfix_(41238).exe` (the `-1/-2/-3.bin` parts next to it) or
  picks it with a file dialog.
- All user data lives in `~/MirrorsEdge/` (see layout).

- The launcher is the installer (user, 2026-11-08). One app, two states: "not installed" shows the drop zone and, once
  started, a step list with a progress bar, current step, elapsed time and a live log tail; "installed" shows the
  existing Play and settings tabs. A "Repair / Reinstall" action stays in the Paths tab.

- Scope (user, 2026-11-08): ship the existing launcher, plus whatever else is needed, together with our code in the DMG.
  So the work is packaging and an install flow added to `MELauncher.swift`, not a new app.

## First-run wizard (user, 2026-11-08)

1. **"Do you have the game?"** Welcome screen. We say we need their own legal GOG copy of *Mirror's Edge* (version 1.01)
   and ship nothing from it. Buttons: "I have the installer" / "Where do I get it?" (opens the GOG page).
2. **Drag and drop** the GOG `setup_*.exe` (the `-1/-2/-3.bin` parts must sit next to it; say so if they are missing).
   Check the file and its parts before going on.
3. **Where it is stored:** show `~/MirrorsEdge/` with a "Change..." button, the space needed (about 15 GB) and the
   space free. Nothing is written until they confirm.
4. **Command Line Tools prompt:** if `xcode-select -p` fails, explain why we need them (they compile the game on this
   Mac) and offer a button that runs `xcode-select --install`; wait until they are installed, then continue.
5. **Install:** step list with progress, time and log (see above). Resumable; Cancel is safe.
6. **Done:** switch to the Play view.

## Delivery: GitHub Actions ships the DMG (issue #15)

A workflow on a macOS arm64 runner builds DXVK from the patch, the libLLVM and helper tools, and the launcher, then
assembles the DMG and attaches it to a GitHub release (tag push or manual run). No game files are on the runner. We
test by installing the released DMG on a clean Mac with only the Command Line Tools and a real GOG copy.

## Status (2026-11-09): v0.1.0 released

Released as `v0.1.0` (tag on the merge of PR #18; issues #8 and #15 closed). Built by `.github/workflows/macos-dmg.yml` on a
`macos-15` runner (about 4 minutes); it runs only by hand (Actions > Run workflow, pick a branch) or on a `v*` tag, and a tag
also attaches the DMG to a GitHub release. The default config of a new install is Native (no upscaling) with PhysX effects off.

Tested on the dev Mac (M1 Max, macOS 15):
- `Installer.swift` + `SetupView.swift` (wizard) in the launcher; headless `MELauncher --install <setup.exe> [--base dir]`.
  Two full headless installs from the real GOG installer (stock `/usr/bin/python3` 3.9, Apple clang, bundled-style SDL3;
  the second from the packaged app with an empty environment) produced a game that starts and renders (60-70 s bounded runs).
  Layout under `<base>`: `pc/` (game), `pc-deps/`, `pc-recomp/` (catalogs, lifted C, `bin-macos/me_pc`, `lib/libSDL3`),
  `dxvk/bin/`, `pc-user/`, `cache/`, `logs/`, `install.json` (finished steps; a failed install resumes).
- `make-dmg.sh` + `bundle_dylibs.py`: 80 MB DMG; fails the build if a Homebrew path remains, checks the helper tools run with an
  empty environment and that libLLVM loads with the x86 disassembler. Ad hoc signed. The CI run built the patched DXVK from a
  clean v3.1.1 and the DMG on the first try.

Not tested yet:
- **A Mac without Homebrew** (planned: M1 MacBook Air, from the release). The dev Mac hides mistakes in the bundled MoltenVK ICD
  / Vulkan loader lookup (`ME_PC_BUNDLE`, `SDL_VULKAN_LIBRARY`), because Homebrew is a fallback there. The launcher sets
  `ME_PC_BUNDLE` only when `Resources/vendor/lib/libvulkan.1.dylib` exists.
- The wizard end to end by a user (the look was checked by the user at a glance; Command Line Tools prompt, cancel and
  resume, a wrong installer file); a long play session on an installed build; notarisation.
- The lift step has only been run with Apple clang 17 and Python 3.9 on macOS 15.

## Layout (as built)

```
~/MirrorsEdge/
  pc/            innoextract output (Binaries/, __redist/ ...), 11 GB
  pc-deps/       msvcr80/msvcp80, rebased DLLs
  pc-recomp/     catalogs, lifted C, objects, bin-macos/me_pc, lib/libSDL3 (about 1.1 GB)
  dxvk/bin/      the DXVK dylib copied from the app
  pc-user/       what the game writes: settings, saves
  cache/         created; the warm-up cache still goes to ~/.cache/dxvk (not yet pointed here)
  install.json   finished steps, so a failed install resumes
  logs/          one log per step
```

## What the DMG ships (no game content)

In `MirrorsEdgeLauncher.app/Contents/Resources/`: lifter sources (`pc/tools/*.py`), runtime sources (`pc/runtime/`),
Shatter sources, `libdxvk_d3d9.0.dylib` (built by CI from the patch), MoltenVK, vulkan-loader, SDL3 dylibs, `innoextract`,
`cabextract`, `msiextract` (or a small Swift/Python replacement), and a private Python. The python scripts run with `-I`.

## Decision: libLLVM (2026-11-08)

CI runs `brew install llvm@23` and copies `libLLVM.23.1.dylib` and `libLLVM-C.dylib` into the app, with their install
names fixed (`install_name_tool`) and a ctypes load test. The DMG carries the licence files of everything bundled
(`Resources/licenses/`): LLVM (Apache-2.0 with LLVM exceptions), SDL3 (zlib), MoltenVK and Vulkan loader (Apache-2.0),
DXVK (zlib), innoextract (zlib), cabextract (GPL-2.0) and msitools (GPL/LGPL; the GPL tools also need a source offer
or link). A stripped X86-only build can replace this later if the size matters.

Runtime paths: `ME_PC_BUNDLE` (set by the launcher to `Resources/vendor`) is checked first for the MoltenVK ICD json
and the Vulkan loader (`rt_host_macos.c`); DXVK's loader takes `SDL_VULKAN_LIBRARY`, falling back to Homebrew.
SDL3: done. `build_pc_runtime.py --native --libllvm <dir> --sdl3-bundle --work <w>` compiles with `/usr/bin/clang`,
copies libSDL3 to `<w>/lib` (install name `@rpath/libSDL3.0.dylib`, ad hoc re-signed) and links `me_pc` with rpath
`@executable_path/../lib`; `otool -L` shows no Homebrew path, and a 60 s bounded run rendered the level. The bundled
SDL3 has no other non-system dependencies. Not yet tested: `ME_PC_BUNDLE` (the ICD/loader lookup) and the lift step
with `--native`.

## Findings that block "CLT only"

1. **The lifter needs libLLVM.** `pc/tools/x86cfg.py` (`LLVMDisasm`) loads `libLLVM` through ctypes for x86
   disassembly. Xcode CLT does not ship a loadable `libLLVM.dylib`. Options: bundle a stripped X86-only `libLLVM.dylib`
   (about 60-100 MB, needs a CI build of LLVM) or replace the disassembler with a Python/C one (Capstone, or the lifter's
   own decoder). Bundling is the cheaper path.
2. **Compiling with Apple clang: works (tested 2026-11-08).** Apple clang 17 compiled all 248 units of the existing lifted C
   without errors and linked `me_pc` in 299 s (M1 Max, 10 jobs). A 90 s bounded tutorial run (`run-bounded.sh`) started, loaded the level
   and rendered, with no crash or `mvk-error`. Not tested: a long play session, the lift step itself on Apple clang (only
   the compile was). Still to do: a `--cc` option so no fake LLVM prefix is needed (the test used a prefix whose `bin/clang`
   links to `/usr/bin/clang`). The old note follows.
   Old note: `build_pc_runtime.py --macos <dir>` takes `<dir>/bin/clang` from
   Homebrew `llvm@23`. The generated C and runtime must be built with `/usr/bin/clang` (Apple clang 17) and checked:
   Apple's default CPU has LRCPC (see the comment at `build_pc_runtime.py:136`), compile time, and output behaviour.
   Needs a `--cc` option and a full run.
3. **Hardcoded Homebrew paths.** `rt_host_macos.c:249` (MoltenVK ICD json) and `:878` (`/opt/homebrew/lib/libvulkan.1.dylib`)
   must become bundle-relative, via an env var set by the launcher.
4. **Build time and size.** Lift + compile is 7-8 min on an M1 Max and the extraction is 11 GB. The app needs a progress UI
   with per-step ETA and a free-space check (about 15 GB).

## Install steps (each idempotent, recorded in `install.json`)

1. Check: Apple Silicon, CLT, free space, a GOG exe whose parts are present.
2. `innoextract` -> `game/`; verify `MirrorsEdge.exe` SHA-256 `58de3df2...a3dd`; copy `PhysXCore.bak` to `PhysXCore.dll`.
3. Extract msvcr80/msvcp80 from vcredist; verify the two SHA-256s.
4. Rebase the 11 DLLs (table in `build_pc_runtime.py`).
5. Catalogs (game, msvcr80, msvcp80, 11 DLLs).
6. Lift and compile -> `build/bin-macos/me_pc`.
7. Done: the main window shows Play and the existing settings tabs.

Stream each tool's output to `logs/` and parse step markers for the progress bar. Reinstall = "Repair" button (re-run the
failed step or all). Uninstall = delete `~/MirrorsEdge/` (offer in the UI).

## Work items

1. `Installer.swift` (or a Python driver called by the app): the steps above, progress model, resume.
2. Launcher UI: first-run drop zone, progress view, error view with the log; `Paths` defaults point into `~/MirrorsEdge/`.
3. Make the lifter run on a bundled libLLVM; test Apple clang for the compile (findings 1 and 2).
4. Bundle-relative dylib/ICD paths in the runtime (finding 3).
5. `pc/launcher/macos/make-dmg.sh`: assemble the bundle, `codesign` (ad hoc at least), `hdiutil create` with an
   Applications symlink and a background image.
6. GitHub Actions (#15): macOS arm64 runner builds DXVK, runtime pieces, libLLVM, the launcher; makes the DMG; attaches it
   to a release. Notarisation needs an Apple Developer account; without it users right-click > Open.
7. Test on a clean user account/Mac with only CLT.

## Open

- Is a DMG that contains the lifter and runtime source (but no game data) fine under the licence? The repo already
  publishes them, so it should be.
- Gatekeeper: ad hoc signing is enough for local builds, but the downloaded DMG is quarantined. Decide on notarisation.
- GOG installer variants other than 1.01 are not supported (the lifted code is tied to one executable hash).
