# macOS MetalFX spatial scaler spike

## Standalone API proof

`main.swift` creates an `MTLFXSpatialScaler` and presents its output through a `CAMetalLayer`; `build.sh` writes the app executable under the ignored `work/` directory. On this Mac (Apple GPU, macOS 15.8), the Swift source compiled against MetalFX and a run with `work/frames/frame_900.ppm` presented 120 frames successfully.

The observed PPM is 960x600, while this first harness version configures 640x416 input and 1280x832 output. Consequently, the run used an uninitialized input texture and the harness printed a size mismatch. The 120-present result proves API/device/layer setup only; it does not establish image quality or valid scaling. No visual quality comparison was made. The requested quality result remains open.

## DXVK integration investigation

The current DXVK path reaches `Presenter::presentImage` in `src/dxvk/dxvk_presenter.cpp` after the D3D9 backbuffer has been copied into the swapchain image by `DxvkSwapchainBlitter`. `presentImage` has the acquired swapchain image index and calls `vkQueuePresentKHR`, but it does not have the original D3D9 backbuffer image. The actual scaling copy is encoded in `src/dxvk/dxvk_swapchain_blitter.cpp` (the caller has both source and destination Vulkan images).

`VK_EXT_metal_objects` is present in the installed MoltenVK headers. `vkExportMetalObjectsEXT` can export an `MTLCommandQueue` and Vulkan image textures. However, inserting a MetalFX command buffer at the presenter would be too late to scale the internal-resolution source, while inserting it in the blitter requires establishing the correct submission ordering, image layouts/usages, drawable texture lifetime, and present synchronization. That path was not implemented or validated in this spike. No evidence was collected about MoltenVK drawable ownership at `vkQueuePresentKHR`.

The requested fallback (DXVK offscreen image with host-owned CAMetalLayer presentation) was not attempted because it would replace DXVK's SDL/Vulkan WSI lifecycle and is not a small fallback after the first route's source-image/lifetime issue.

## Game run and measurements

No game process was launched for this spike. Therefore there are no measured native versus 50% + MetalFX frame times, no MetalFX game dump, and no `mvk-error`/lost-device result. The 120 standalone presents are not a frame-time benchmark (the timer requests 60 Hz and does not record GPU completion timings).

`ME_PC_UPSCALER` modes (`native|dxvk|metalfx|fsr`) are not wired into the runtime, launcher, or DXVK. The existing behavior remains the current DXVK present blit. `dxvk-3.1.1-macos-metalfx.patch` is intentionally absent because there is no integration delta to capture.

## Recommendation

The platform API is available and a small standalone scaler can be created. Before game integration, make the standalone harness consume a correctly sized game frame, save both MetalFX and bilinear outputs, and inspect them. Then prototype within `DxvkSwapchainBlitter::present`, where the source and destination Vulkan images are both available; only proceed if synchronization and drawable texture export work without changing DXVK's presentation ownership. Do not claim performance or image-quality gains from the current result.
