# FSR 1.0 DXVK spike

Status: **not implemented**. No FSR shader, runtime selector, patch delta, game run, image comparison, or frame-time measurement was produced in this spike. Do not use this directory as evidence that FSR works.

## Findings

The isolated source copy is `work/dxvk-fsr/dxvk-3.1.1-git`, made from `work/dxvk-defer/dxvk-3.1.1-git`; the original was not modified. Its inherited working tree contains the existing macOS/session fixes. Its existing `build-macos/src/d3d9/libdxvk_d3d9.0.dylib` predates any FSR changes and is not an FSR build.

DXVK's present image scaling occurs in `DxvkSwapchainBlitter::present` and `performDraw` in `src/dxvk/dxvk_swapchain_blitter.cpp`, using built-in graphics pipelines. Presentation begins dynamic rendering before the blit, then composites HUD/cursor. A two-pass EASU/RCAS implementation needs intermediate image lifetime management, compute pipelines, descriptors, barriers/layout transitions, and correctly ordered HUD/cursor composition. Replacing the current fragment shader alone would not implement the requested compute passes.

DXVK's Meson setup already finds `glslang` or `glslangValidator` and compiles GLSL sources listed in `src/dxvk/meson.build` using Vulkan 1.3 targeting. This is a verified build-path observation; compute pipeline/resource integration was not implemented or tested.

The provided `/Users/pradeesh/orca/workspaces/RunnerRecompiled/stingaree/work/run-bounded.sh` is not directly usable from this checkout: it changes directory to `/Users/pradeesh/orca/workspaces/RunnerRecompiled/text-colour-debug`, defaults to the `dxvk-textfix` dylib, and assumes that checkout's relative `work/` paths. No game process was launched, and no `me_pc` process was observed at inspection time. Consequently there are no measured image-quality or frame-time results at native, 50%, or 67% scale.

## License

No AMD FidelityFX source or shader code was incorporated. Any implementation that adds FidelityFX Super Resolution 1.0 must preserve AMD's MIT license header and include the license in this spike's documentation.
