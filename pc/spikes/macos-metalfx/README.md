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

## DXVK integration attempt (round 2)

The experiment was built in `work/dxvk-metalfx`, copied from the current `work/dxvk-defer/dxvk-3.1.1-git`; the original defer tree was not modified. The integration prototype carries the D3D9 source image into DXVK's ordered submission/present worker, exports the source, acquired swapchain image, and Metal command queue through `VK_EXT_metal_objects`, then encodes MetalFX into an intermediate Metal texture and blits that to the acquired image before `vkQueuePresentKHR`. WSI ownership remains with DXVK.

The prototype compiles successfully as an arm64 DXVK dylib. It requires Objective-C++ compilation and links Metal, MetalFX and Objective-C runtime frameworks. The attempted run used `ME_PC_UPSCALER=metalfx`, `ME_PC_RENDER_SCALE=0.5`, and the bounded tutorial helper. Drawable/source dimensions were observed as 1728x972 and 1280x720 respectively. An initial prototype rejected the non-2x ratio; that local guard was removed before the subsequent run.

The subsequent run did **not** run MetalFX. Exact blocker from `work/metalfx-game3/run.log`, emitted while initializing the DXVK device: `MetalFX requested, but VK_EXT_metal_objects is not advertised by the Vulkan device.` At the present hook, the corresponding message was `DXVK MetalFX: blocked: vkExportMetalObjectsEXT unavailable`. DXVK's loader therefore has no extension function pointer and cannot export the queue or textures. The extension is declared in the installed Vulkan headers but was absent from MoltenVK's device extension enumeration in this process. No swapchain usage or drawable-ownership failure was reached, because texture export did not start. We did not attempt the larger host-owned CAMetalLayer redesign.

The bounded helper ended after 65 seconds. The log contains no `VK_ERROR_DEVICE_LOST` or `mvk-error`; one unrelated `[mvk-warn] VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart.` was reported. Dumped frames 300 and 900 came from the ordinary DXVK blit fallback, not MetalFX. During that fallback run, the runtime reported about 60 fps / 16.7 ms per frame in the menu phase, then 29.8–30.3 fps / 33.0–33.6 ms in the short transition/loading interval and one 9.57 s frame. These are not a controlled native-vs-upscaler comparison and do not establish MetalFX performance. The second 25-second run confirmed the same extension-enumeration failure.

`pc/dxvk/dxvk-3.1.1-macos-metalfx.patch` records the experimental delta against the current patched DXVK source. It is a blocked prototype, not a working upscaler patch; do not apply it as a production feature while MoltenVK fails to advertise the required extension.
