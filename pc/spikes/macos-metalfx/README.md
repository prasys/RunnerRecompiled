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

## Final-output readback comparison (round 4, job 1)

The DXVK bridge supports `ME_PC_METALFX_DUMP=<dir>` together with the existing `ME_PC_DUMP_FRAMES` selection. For MetalFX frames it reads back the scaled output intermediate after the scaler and before the swapchain blit. With `ME_PC_UPSCALER` unset it reads back the acquired swapchain texture after DXVK's bilinear blit. Both use the DXVK present frame ID; PPM captures stay in `work/` and can be converted with `sips`.

For a comparable capture, both bounded runs used 50% internal scale, frame 300 and an empty `ME_PC_AUTO_KEYS` (the same static EA startup-logo screen). The images are both 1728x972:

- `work/metalfx-out/frame_300_metalfx-output.png`
- `work/metalfx-out/frame_300_dxvk-final.png`
- 2x crops: `work/metalfx-out/frame_300_metalfx-crop.png`, `work/metalfx-out/frame_300_dxvk-crop.png`

Visual inspection of the full images and the 2x crop: MetalFX preserves the logo's diagonal and curved high-contrast edges slightly more crisply than the plain DXVK bilinear blit. The improvement is visible but modest on this static logo; it does not establish quality across game scenes. These PNGs were converted from the selected PPM readbacks with `sips` and inspected directly.

Two attempted tutorial captures at frames 1400 and 1800 were not suitable for image comparison: frame 1400 landed on different loading/game states in the two runs, and the unset run for frame 1800 lost the Vulkan device during the tutorial-load pipeline warm-up before reaching that frame. No readback was produced for the latter. The stable frame-300 pair is the only Round 4 final-output comparison reported above.

## Metal validation follow-up (round 4, job 2)

The Round 3 warning was reproduced under `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog`. MetalTools reported `The following Metal object is being destroyed while still required to be alive by the command buffer ... (label: vkQueueSubmit MTLCommandBuffer on Queue 0-0)`, followed by an `AGXG13XFamilyBuffer` with no label and length 67,108,864 bytes (one run later showed 33,554,432 bytes). The exported source and swapchain textures both reported `texture.buffer == nil`; our labeled MetalFX intermediate and labeled readback buffers are distinct objects. The warning therefore identifies a Vulkan/MoltenVK backing buffer, not a MetalFX output or buffer-backed exported texture. MetalTools did not expose which Vulkan object owns the backing allocation.

With `MTL_DEBUG_LAYER_ERROR_MODE=nslog`, one failing run also emitted an Objective-C exception and stack: `-[MTLDebugCommandBuffer unlockPurgeableObjects]` -> `preCompletionHandlers` -> `MTLToolsCommandBuffer invokeCompletedHandlers` -> `-[ _MTLCommandBuffer didCompleteWithStartTime:endTime:error:]` -> `IOGPUMetalCommandBuffer didComplete...`; the exception was `-[MTLSharedEventListenerInternal unlockPurgeableState]: unrecognized selector`. Other reproductions reached the host SIGSEGV handler at `me_pc` image offset `+0x9820` (`atos`: `f_78131ACA + 1272`). No matching crash report was produced because the runtime catches that signal itself. A validation-only unset-upscaler run completed 75 seconds without the warning, which ties the reproduced warning to the direct MetalFX integration path in these runs.

The integration now carries an `Rc<DxvkImage>` for the source through the asynchronous DXVK present entry and keeps a local `Rc<DxvkImage>` for the acquired destination until the external Metal command buffer completes. It reuses a labeled full-resolution MetalFX intermediate until its dimensions change, labels its own readback buffers and Metal command buffer, and waits for the MetalFX command buffer on MoltenVK's exported queue. The exported source/destination textures are labeled too. An earlier per-frame `vkDeviceWaitIdle` experiment and a 5000-submission deferred-destroy margin did not consistently eliminate the warning, so neither is in the final path.

The final implementation build completed a 75-second validation tutorial run (`work/metalfx-r4-validation-persistent/run.log`) and a 75-second normal tutorial run (`work/metalfx-r4-normal-final/run.log`). Both reached the tutorial and ended at the bounded timeout with no deallocated-object warning, `mvk-error`, `VK_ERROR_DEVICE_LOST`, or host signal. Playable sections reported about 59–60 fps in the normal run; this remains capped runtime frame reporting, not an uncapped GPU benchmark. Earlier intermediate Round 4 builds did reproduce the warning, and one normal run lost the device at the tutorial transition, so the clean final pair is evidence for the latest lifetime changes, not proof across all runs or scenes.

**Recommendation:** keep the source-image references and persistent labeled intermediate. Treat the 75-second clean validation and normal runs as a successful spike result, then repeat validation during longer play and swapchain recreation before calling this production-stable. The exact Vulkan allocation behind the earlier unlabeled buffer remains unidentified.

## Launcher test (Claude, session 5)

The launcher (`pc/launcher/macos/MELauncher.swift`) has an Upscaling picker (Native / Standard / MetalFX). With MetalFX it sets
`ME_PC_UPSCALER=metalfx`, `ME_PC_RENDER_SCALE` from the slider and `ME_PC_DXVK` to the "DXVK (MetalFX)" dylib. Tested headless
(`MELauncher --launch` with a config JSON, a copy of the user folder, 75 s bounded, scale 50%): the log shows
`Enabling advertised VK_EXT_metal_objects` and `DXVK MetalFX: encoding 1280x720 -> 1728x972`, no `mvk-error`, no
`VK_ERROR_DEVICE_LOST`, median 59 fps (frame log, not GPU timing), one loading hitch down to 3.5 fps. Not tested: the picker
in the GUI, the picture itself. A first run used the stale `build-macos` dylib (no MetalFX code) and ended in
`VK_ERROR_DEVICE_LOST` in the plain DXVK path; the MetalFX build is `build-spike`.

## Folded into the main patch (2026-11-08, Claude)

`pc/dxvk/dxvk-3.1.1-macos-metalfx.patch` is gone: its hunks are appended to `pc/dxvk/dxvk-3.1.1-macos.patch`, so one
dylib (`pc/dxvk/build-macos.sh`) has the fixes, the map-tagged pipeline warm-up and MetalFX (`ME_PC_UPSCALER=metalfx`
selects it; unset = plain DXVK blit). Checked: the combined patch applies to a clean v3.1.1 (with the nested
`dxbc-spirv/submodules/spirv_headers` submodule), builds, and an 80 s bounded run with `ME_PC_UPSCALER=metalfx
ME_PC_RENDER_SCALE=0.5` logs both `DXVK MetalFX: encoding 1280x720 -> 1728x972` and `warm-up map: tdmainmenu`
(26 recorded states, early shader creation), no `mvk-error`, no `VK_ERROR_DEVICE_LOST`. The same run froze at the
`escape_p` load (1.3 fps, 3.7 s frame): that map has no recorded states, the known level-start stall (#1/#9), not MetalFX.
Not done: the launcher still points "DXVK (MetalFX)" at `work/dxvk-metalfx/build-spike`; it should use the combined
build (`work/dxvk/bin/`). Plain DXVK vs MetalFX frame time on a recorded map is not yet compared.
