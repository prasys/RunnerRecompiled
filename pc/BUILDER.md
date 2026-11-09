# RunnerBuilder

A program that turns **your own GOG copy** of Mirror's Edge into a folder ready to copy to a tablet: your personal APK, a ZIP of your game files, an optional GPU driver and short instructions. You point it at the game folder and press Build.

The program itself holds no game code and no game files. **What it produces does: keep the result to yourself, do not publish or share it.**

## How it is put together

| Part | What it does |
|---|---|
| `pc/tools/builder.py` | The core: checks the game, downloads the tools, runs the build, prepares the folder. Lines starting with `==` name the current stage. |
| `pc/tools/selfbuild.py` | The build itself (see [SELFBUILD.md](SELFBUILD.md)). |
| `pc/tools/redist.py` | Takes the Visual C++ 2005 and PhysX 2.8.0 DLLs out of the official installers without running them. Used only when they are not already on the PC. |
| `pc/tools/builder-gui/RunnerBuilder.cs` | The Windows window. Collects the two paths, runs the core, shows its output, checks for a new version. No build logic. |
| `pc/tools/builder-gui/make_exe.py` | Compiles the window with the `csc.exe` that ships with Windows and embeds the sources and the prebuilt third-party libraries. |

The game ZIP is about 11.5 GB. Tools go to `%LOCALAPPDATA%\RunnerBuilder`, or `C:\RunnerBuilder` when that path has non-ASCII letters, because `aapt2` cannot open those. The signing key `personal-key.p12` lives there as well: with the same key a new APK installs over the old one.

To build the exe:

```
python -I pc/tools/builder-gui/make_exe.py --dxvk libdxvk_d3d9.so --adrenotools <folder> --turnip turnip-*.zip --out <folder>
```

## New versions

At start the program reads `version-builder.json` from the root of the repository and offers to open the project page when `versionCode` is greater than `VersionCode` in `RunnerBuilder.cs`. Any failure is silent. To release: raise both numbers.

## Linux and macOS (planned)

Windows is the only system that works today. The split above exists so that another system needs its own window and the items below, not a second builder. None of this has been tried yet.

1. **Tools.** A new entry in `HOSTS` in `builder.py`. Google's repository lists `android-ndk-r30-linux.zip`, `android-ndk-r30-darwin.zip`, `build-tools_r35_linux.zip` and `build-tools_r35_macosx.zip`; llvm-mingw and JDK archives still have to be chosen. Unpacking has to keep the executable bit, which `zipfile` does not do.
2. **Disassembler.** `x86cfg.LLVMDisasm` loads `libLLVM-*.dll`; it has to find `.so` or `.dylib` too.
3. **APK packing.** `android/build.ps1` has to become Python, or PowerShell becomes a requirement.
4. **Visual C++ 2005 and PhysX 2.8.0.** `redist.py` already unpacks them without running the installers, but calls `expand.exe` and the `tar.exe` of Windows. Other systems need replacements that read LZX cabinets and 7-Zip (for example cabextract and 7z).
5. **The game.** On macOS, and on many Linux machines, there is no installed Windows game. The builder should also accept the GOG offline installer and unpack it without running it (innoextract). A folder from Wine or Heroic should work as it is.
6. **Window.** A small Python and tkinter window, using the same core and the same `version-builder.json`. On macOS the app is unsigned, so Gatekeeper will object; Apple Silicon needs checking against the x86_64 tools.
