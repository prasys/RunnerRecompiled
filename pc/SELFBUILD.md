# Building the PC version for Android from your own copy

This repository holds no game code and no game files. `pc/tools/selfbuild.py` turns **your own GOG copy** of Mirror's Edge into a personal APK on your own PC.

**The APK and the build folder contain code derived from your copy of the game. Keep them to yourself: do not publish or share them.**

The program described in [BUILDER.md](BUILDER.md) does all of this for you on Windows. This page is the manual way.

## What you need

- A Windows 10/11 x64 PC with about 2 GB free for the build. It took about 10 minutes on a 20-thread CPU with 32 GB of RAM; expect longer on fewer cores. Less memory was not tested.
- **Mirror's Edge from GOG**, installed. Other releases will not work: the build checks the SHA-256 of `MirrorsEdge.exe`, and Steam, EA and retail executables differ or are protected.
- **Microsoft Visual C++ 2005 Redistributable (x86)**, current version, so that `msvcr80.dll` and `msvcp80.dll` are in `WinSxS` (builds 8.0.50727.9680 and 8.0.50727.6195 are accepted).
- **NVIDIA PhysX System Software 9.23.1019**, which installs `Engine\v2.8.0` under `Program Files (x86)\NVIDIA Corporation\PhysX`.
- Python 3.10 or newer.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (ucrt, x86_64).
- Android NDK r30, and the Android SDK with `platforms;android-35` and `build-tools;35.0.0`.
- JDK 17 or newer.
- `libdxvk_d3d9.so`: DXVK Native for Android, built as described in [dxvk/README.md](dxvk/README.md).

## Build

From the root of this repository:

```
python -I pc/tools/selfbuild.py --game "C:/GOG Games/Mirror's Edge" --dxvk path/to/libdxvk_d3d9.so ^
    --toolchain path/to/llvm-mingw --ndk path/to/android-ndk --sdk path/to/android-sdk --jdk path/to/jdk ^
    --workspace selfbuild
```

Every input is checked first, and the script stops with a message naming what is missing or different. If the libraries are not installed, `--crt` and `--physx` point at folders that hold them. The result is `selfbuild/RunnerRecompiled-pc-personal.apk`. A later run reuses what is already built.

## Install

1. Install the APK on an Android 10+ device with an ARMv8.4 CPU and Vulkan. The first version was tested on a Snapdragon 8 Elite (Adreno 830).
2. Zip your game folder (`Binaries`, `Engine` and `TdGame` inside; about 12 GB free is needed on the device) and copy the ZIP to the device, or copy the unpacked folder.
3. Open the app and choose the ZIP or folder in the setup screen.

Reporting a problem: **Settings → Support → Send logs**, right after it happens and before starting the game again.

## Credits

The high-resolution font page fix follows the analysis in [Mirror's Edge Tweaks](https://github.com/softsoundd/MirrorsEdgeTweaks) by softsoundd; it is reimplemented in `pc/runtime/rt_ue3.c`.
