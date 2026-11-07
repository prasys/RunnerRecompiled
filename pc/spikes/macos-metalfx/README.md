# macOS MetalFX spatial scaler spike

## Standalone API proof and image comparison

`main.swift` loads the saved P6 PPM `work/frames/frame_900.ppm` (960x600) into an input texture, creates a MetalFX spatial scaler to 1920x1200, and saves its output as PNG. It also makes a plain CPU bilinear 2x enlargement of the same PPM as a reference. `build.sh` compiles the harness and runs it; generated binary and PNGs remain under ignored `work/`.

On this Mac (Apple GPU, macOS 15.8), the harness compiled and ran successfully. Both output images are 1920x1200:

- `work/metalfx-out/frame_900_metalfx_1920x1200.png`
- `work/metalfx-out/frame_900_bilinear_1920x1200.png`

Visual inspection: the images are very similar at this size. MetalFX looks slightly crisper at a few high-contrast edges; the difference is subtle in this static frame. This is a qualitative inspection of one frame, not a general quality result or performance measurement. The scaler API and real input/output path are now exercised.

## DXVK integration investigation

The current DXVK path reaches `Presenter::presentImage` in `src/dxvk/dxvk_presenter.cpp` after the D3D9 backbuffer has been copied into the swapchain image by `DxvkSwapchainBlitter`. `presentImage` has the acquired swapchain image index and calls `vkQueuePresentKHR`, but it does not have the original D3D9 backbuffer image. The actual scaling copy is encoded in `src/dxvk/dxvk_swapchain_blitter.cpp` (the caller has both source and destination Vulkan images).

`VK_EXT_metal_objects` is present in the installed MoltenVK headers. `vkExportMetalObjectsEXT` can export an `MTLCommandQueue` and Vulkan image textures. However, inserting a MetalFX command buffer at the presenter would be too late to scale the internal-resolution source, while inserting it in the blitter requires establishing the correct submission ordering, image layouts/usages, drawable texture lifetime, and present synchronization. That path was not implemented or validated in this spike. No evidence was collected about MoltenVK drawable ownership at `vkQueuePresentKHR`.

The requested fallback (DXVK offscreen image with host-owned CAMetalLayer presentation) was not attempted because it would replace DXVK's SDL/Vulkan WSI lifecycle and is not a small fallback after the first route's source-image/lifetime issue.

## Game run and measurements

No game process was launched for this spike. Therefore there are no measured native versus 50% + MetalFX frame times, no MetalFX game dump, and no `mvk-error`/lost-device result. The 120 standalone presents are not a frame-time benchmark (the timer requests 60 Hz and does not record GPU completion timings).

`ME_PC_UPSCALER` modes (`native|dxvk|metalfx|fsr`) are not wired into the runtime, launcher, or DXVK. The existing behavior remains the current DXVK present blit. `dxvk-3.1.1-macos-metalfx.patch` is intentionally absent because there is no integration delta to capture.

## Recommendation

The platform API is available and a small standalone scaler can be created. Before game integration, make the standalone harness consume a correctly sized game frame, save both MetalFX and bilinear outputs, and inspect them. Then prototype within `DxvkSwapchainBlitter::present`, where the source and destination Vulkan images are both available; only proceed if synchronization and drawable texture export work without changing DXVK's presentation ownership. Do not claim performance or image-quality gains from the current result.
