# macOS MetalFX spatial scaler spike

## Standalone quality check

The standalone harness loads `work/frames/frame_900.ppm` (960x600) as the input texture and writes a 1920x1200 MetalFX spatial-scaled PNG and a same-frame CPU bilinear PNG:

- `work/metalfx-out/frame_900_metalfx_1920x1200.png`
- `work/metalfx-out/frame_900_bilinear_1920x1200.png`

Both were inspected on this Mac (Apple GPU, macOS 15.8). The MetalFX image looks slightly crisper at a few high-contrast edges; the difference is subtle. This is a qualitative judgment from one static frame, not a general quality result or performance measurement.

## DXVK prototype

The prototype lives in ignored `work/dxvk-metalfx`, copied from `work/dxvk-defer/dxvk-3.1.1-git`. The original defer and upstream trees were not edited. The code enables `VK_EXT_metal_objects` only when requested and when `vkEnumerateDeviceExtensionProperties` reports it, loads `vkExportMetalObjectsEXT`, carries the D3D9 source image into the present worker, then exports the source texture, acquired swapchain texture and same-queue `MTLCommandQueue`. It creates a MetalFX output intermediate and blits that to the acquired swapchain image before DXVK calls `vkQueuePresentKHR`; DXVK retains WSI ownership.

The route supports the observed 1280x720 source to 1728x972 drawable ratio. The earlier 2x-only check was removed. The Objective-C++ bridge built successfully as an arm64 DXVK dylib.

## Bounded game runs

Runs used `work/run-bounded.sh` for 75 seconds with `ME_PC_RENDER_SCALE=0.5`; this requested a 1728x972 window for the 1280x720 guest resolution, below the display's 3024x1964 size. `ME_PC_UPSCALER=metalfx` was set for the MetalFX run. The current run (`work/metalfx-run9/run.log`) logged `DXVK MetalFX: encoding 1280x720 -> 1728x972`, reached the tutorial, and ended normally after 75 seconds. It had no `VK_ERROR_DEVICE_LOST` or `mvk-error`. It did log the MoltenVK warning `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart.`

A same-scale unset-upscaler baseline (`work/dxvk-run-baseline`) also ran for 75 seconds and reached the tutorial. In the playable tutorial sections, the unset baseline reported roughly 59–60 fps (about 16.7–17.0 ms/frame); the MetalFX run reported roughly 59–60 fps (about 16.7–17.0 ms/frame). These are runtime frame-log averages, not isolated GPU timings, and the game appears capped near 60 fps. Both runs had transition/loading spikes; the MetalFX run included isolated ~1.0 second frames. This does not show a measurable frame-time advantage at the cap.

`work/metalfx-run9/frame_300.ppm`, `frame_900.ppm`, `frame_1300.ppm`, `frame_1400.ppm` and `frame_1800.ppm` were captured with `ME_PC_DUMP_FRAMES`. They are D3D9 backbuffer captures taken before the MetalFX present pass, so they confirm rendered content but cannot compare the final MetalFX output against the DXVK blit. No in-game output-quality comparison was measured.

A separate run with Metal API validation enabled reached the tutorial but then reported `command buffer references deallocated object which previously existed` for a 64 MiB resource and terminated with signal 11. Adding retained references and waiting for the Metal command buffer to complete did not eliminate that validation failure. The non-validation bounded run completed, but this unresolved lifetime warning means the prototype is not yet reliable enough to call stable. The exact offending object was not identified. The captured run logs are under ignored `work/metalfx-run6`, `work/metalfx-run7`, and `work/metalfx-run8`.

## Findings and recommendation

The initial extension blocker was a DXVK logic error: the check examined DXVK's requested-extension list rather than MoltenVK's advertised device-extension list. Querying the real device list enabled the extension and reached actual MetalFX encoding. The direct MetalFX-to-intermediate-to-swapchain route can run through a bounded tutorial session without device loss in a non-validation run, but Metal validation still finds a deallocated-resource reference and crashes. The prototype patch is therefore experimental, not ready to enable by default. Resolve and validate Metal/Vulkan resource lifetime before integrating it as a supported upscaler. No conclusion about in-game image quality is supported by the backbuffer dumps.
