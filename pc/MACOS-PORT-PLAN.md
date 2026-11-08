# macOS port of the PC build (Apple Silicon)

Started 2026-11-06. Route chosen: **A, the PC port on a macOS host** (lifted x86-32 C compiled natively for arm64,
Direct3D 9 through DXVK Native on MoltenVK). Route B (the Xbox build on the full RexGlue runtime, as
[skate3recomp](https://github.com/mchughalex/skate3recomp) does) was set aside: it keeps the emulated Xenos GPU, which
is our bottleneck, and our Adreno-specific patches do not apply on a Mac.

## Why this is a small seam

`pc/runtime/rt.h` declares about a dozen `host_*` functions (memory, mutex, cond, clock, threads, fault handler, files).
`rt_host_win.c` (427 lines) and `rt_host_android.c` (503 lines) implement them. macOS is a third implementation,
`rt_host_macos.c`. The lifter, the catalog and the generated C do not depend on the host. Both hosts load their D3D9
implementation at run time (`LoadLibrary("d3d9.dll")` / `dlopen("libdxvk_d3d9.so")`).

## Phases

| # | Phase | State |
|---|---|---|
| 0 | Tools and game files | **done 2026-11-06** (see `AGENTS.md`, "Local layout") |
| 1 | Lifter and build scripts run on macOS (`libLLVM.dylib`, native arm64 clang) | **done 2026-11-06** (see "Phase 1 results") |
| 2 | Spike: 4 GB guest reservation, 16 KB pages, `mprotect`/fault behaviour on macOS arm64 | **done 2026-11-06** (see "Phase 2 results") |
| 3 | `rt_host_macos.c`: memory, threads, SIGSEGV/SIGBUS handler, files, SDL3 window, input and audio | **written and linking 2026-11-06**; first run reaches `Direct3DCreate9` (see "Phase 3 results") |
| 4 | DXVK 3.1.1 D3D9-only for macOS arm64 on MoltenVK, SDL3 WSI, `dlopen("libdxvk_d3d9.dylib")` | **done: the window shows the picture** (2026-11-06) after a main-thread surface fix. See "Phase 4 results" |
| 5 | Bring-up: build full lifted game, title, menus, movies, sound, first level, PhysX | **in progress**: title, menu and the tutorial level render and play at 60 fps (text/colours, mouse, vsync fixed in session 2). Session 3: wrong-camera flashes fixed by the upstream merge, lost GPU device fixed by deferred destroy in the DXVK patch. Session 4: PhysX fluids and cloth on Shatter's native solvers (issue #2, "Phase 5, session 4"). **Open: multi-second stall when a level starts (shader/pipeline compiles?, issue #1), single ~1 s frames.** See "Phase 5 findings" |
| 6 | Packaging (.app or setup script), README/CHECKLIST | open |

Done criteria per phase:
- 1: `test_x86cfg.py` 17/17; `catalog.py` on `MirrorsEdge.exe` gives 178,605 functions (the Windows result); the
  differential harness passes on arm64 macOS.
- 2: a standalone C test reserves 4 GB, commits/decommits 4 KB-aligned guest ranges, and survives a write fault
  handled in a signal handler. Report what the 16 KB page size forces on `host_commit`/`host_decommit`.
- 4: a standalone D3D9 triangle through the built `libdxvk_d3d9.dylib` on MoltenVK.

## Phase 1 results (2026-11-06)

- `x86cfg.LLVMDisasm` now finds `libLLVM-C.dylib` / `libLLVM.dylib` (pass `--llvm /opt/homebrew/opt/llvm@23/lib`).
  `build_pc_runtime.py --macos /opt/homebrew/opt/llvm@23 --work work/pc-recomp` lifts with that library and compiles
  with its clang (`-arch arm64`). Modules whose binary is missing (PhysXCooking.dll) are skipped; the runtime module
  table is generated from the rest. It stops before linking while `pc/runtime/rt_host_macos.c` does not exist.
- **The catalog is correct on macOS.** Running the tools as of the first lifter commit (`4f2d67e`) on `MirrorsEdge.exe`
  reproduces the documented Windows numbers exactly: 178,605 reachable functions (157,163 code + 19,955 SEH stubs +
  1,487 import thunks), 10,303 jump-table entries. The current tools find more (185,536 reachable functions, 10,304
  tables, 4,700 unresolved indirect jumps) because later commits improved seeding.
- Catalog time on the M-series Mac: about 70 s for the game exe.
- **Full lift and compile on macOS arm64: 0 errors.** Game module: 185,486 functions lifted in 161 shards in 47.5 s,
  0 not lifted, 68 functions containing traps (unsupported instructions). All 14 modules plus the 13 runtime C files that
  exist compiled in about 7.5 min (232 objects, `-O2`, native arm64 clang) with no errors or porting changes to the
  runtime. Output: `work/pc-recomp/gen` 586 MB of C, `obj-macos` 417 MB. Linking waits for `rt_host_macos.c`.
- **The differential harness cannot run on this Mac.** `pc/harness` runs the original x86-32 function and the lifted
  version in one 32-bit Windows process. Apple Silicon cannot execute x86-32 (Rosetta is 64-bit only). Options for
  later: run it on a Windows machine, or write a reference using an x86 emulator (e.g. Unicorn). Until then the
  correctness evidence for lifting on this host is the identical catalog and, at bring-up, the game itself.

## Phase 2 results (2026-11-06)

Spike: `pc/spikes/macos-addrspace/` (written by a Codex agent, results re-run and confirmed by hand).

| Question | Result | Consequence for `rt_host_macos.c` |
|---|---|---|
| 4 GB `PROT_NONE` anonymous reservation | works (no `MAP_NORESERVE` needed) | use `mmap(NULL, 4 GB, PROT_NONE, MAP_PRIVATE\|MAP_ANON)` |
| Fixed low base | hint ignored (got `0x300000000`) | guest pointer = base + u32 offset still works; host pointers must be `uintptr_t`/`void*`, never 32-bit |
| 4 KB `mprotect` | `EINVAL`; host page is 16 KB | commit/protect in 16 KB units; keep per-4 KB guest state in software |
| Zero on recommit | neither `MADV_DONTNEED` nor `MADV_FREE` zeroes | on decommit, re-`mmap(MAP_FIXED\|MAP_ANON)` over whole 16 KB pages (fresh zero pages), or `memset` for partial pages |
| Fault on `PROT_NONE` | delivered as **SIGBUS** (not SIGSEGV); handler can `mprotect` and resume | install handlers for both; PC is `uc->uc_mcontext->__ss.__pc` |
| 64 MB thread stack | works (`pthread_attr_setstacksize`; main thread via `-Wl,-stack_size,0x4000000`) | |

Open question for Phase 3: does the game depend on 4 KB-granular guard pages or `PAGE_NOACCESS` regions? If it does,
those need software checks or a separate mapping scheme. Find out from `VirtualProtect`/`VirtualAlloc` call sites in
`rt_kernel32.c` and the game's use of stack guard pages.

## Phase 3 results (2026-11-06)

`pc/runtime/rt_host_macos.c` implements all ~44 `host_*` functions; `build_pc_runtime.py --macos` links
`work/pc-recomp/bin-macos/me_pc` (Mach-O arm64, 281 MB). Run it from the project root:

```
work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```

- Memory as decided in Phase 2: 4 GB `PROT_NONE` reservation, commit rounded to 16 KB, decommit by `MAP_FIXED` remap.
  Open question resolved: `VirtualProtect` is a no-op in `rt_kernel32.c`, so no software page table is needed.
- `main()` lives in the host file: SDL3 event loop on the main thread (Cocoa requires it), the game on a 64 MB worker
  thread (`rt_main.c`'s `main` is compiled as `me_pc_main`). Windows are created on the main thread; the game gets the
  `SDL_Window*` as its HWND, which is what DXVK Native's SDL back end expects.
- Audio uses an SDL3 audio stream fed by the runtime's own mixer. **OpenAL Soft is not needed.**
- **First run:** all 14 modules load (185,486 lifted game functions), imports resolve, the CRT `DllMain`s run, the game
  entry runs, an SDL gamepad is found, then `Direct3DCreate9` returns NULL (no DXVK yet) and the game dereferences it.
  The SIGBUS handler reported it correctly (guest address 0, last dispatch, host pc). This is the same stage the
  Windows port first reached.
- Not yet exercised: window creation, input, audio, file access beyond the CRT start, threads under load.
- Known gaps: the window is not high-DPI (no `SDL_WINDOW_HIGH_PIXEL_DENSITY`); `host_profile_start` is empty; mouse
  capture is on while the window has focus (`ME_PC_MOUSE_CAPTURE=0` disables it); no key-to-WM_CHAR text path.
- Run-time environment for D3D9: `ME_PC_DXVK=<path to libdxvk_d3d9.dylib>`; `VK_ICD_FILENAMES` is set to the Homebrew
  MoltenVK ICD if it exists; the Homebrew Vulkan loader (`/opt/homebrew/lib`) must be findable by DXVK
  (`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`).

## Phase 4 results (2026-11-06)

DXVK 3.1.1 builds as an arm64 dylib (patch `pc/dxvk/dxvk-3.1.1-macos.patch`, zlib; spike `pc/spikes/macos-dxvk/`, written
by a Codex agent). Source and build output stay in `work/dxvk-mac/`.

- **In the game:** `Direct3DCreate9` works, DXVK finds the Apple M1 Max through MoltenVK, and
  `CreateDevice(adapter 0, type 1, ...) = 0` with a swapchain (D3D9 reports itself as an AMD RX 6700 XT; DXVK's usual
  GPU-ID masking). The game keeps running afterwards (about 128% CPU, 600 MB): render threads start, audio opens,
  the game window (640x360) and a hidden one are created.
- **Frame counter (added): the game presents frames.** `d3d_stat()` (injected into the generated Present, draw,
  Clear and BeginScene thunks by `gen_d3d9_bridge.py`'s `COUNTED` table; `ME_PC_FRAME_LOG=0` turns it off) logs once a
  second. A 110 s run on the M1 Max: about 17 s at 30 fps with 1 draw per frame (start-up, UE3 loading shaders and
  packages), then a steady **~60 fps, ~90 draws and 8 clears per frame** for 5,300+ frames with no crash. The game had
  opened `TdMainMenu.umap`, the menu UI packages, `StartupMovie.bik`, the menu audio and `UI_City.upk`, so this is the
  main menu. (The window was black at this point; see "Phase 5 findings" for why, and what the picture really looks like.)
  (60 fps is probably the game's own cap, so it says nothing about headroom.)
- Not verified (older note): that anything is drawn. A screenshot taken from the terminal showed only the desktop wallpaper
  (screen-recording permission most likely hides other apps' windows), and the runtime logs nothing after start-up.
  Next check: have the runtime log a frame counter/`Present` calls, or look at the window directly.
- **Bug found and fixed:** DXVK's SDL back end asks SDL for Vulkan instance extensions; SDL's own `dlopen` does not
  search `/opt/homebrew/lib`, so `Direct3DCreate9` threw an uncaught `DxvkError`. Fix: `main()` sets
  `SDL_VULKAN_LIBRARY` to `/opt/homebrew/lib/libvulkan.1.dylib` **before** `SDL_Init` (SDL3 snapshots the
  environment, so a later `setenv` is ignored).
- **MoltenVK gaps the Codex agent reported** (from DXVK's log on M1 Max, MoltenVK 1.4.2): no `geometryShader`,
  `shaderCullDistance`, `depthClipEnable`, `robustBufferAccess2`/`nullDescriptor`, no transform feedback; D24S8 and
  D16S8 are substituted with D32S8; no graphics pipeline libraries. The patch makes the first four optional for this
  D3D9-only build. Effects on Mirror's Edge (clip planes, depth clamp, null descriptors) are to be watched for.
- The agent's standalone triangle test did **not** finish: its hand-written SM3 shader bytecode is rejected by DXVK
  (`CreateVertexShader` fails). That is the test's bytecode, not evidence about DXVK; the real game's shaders are the
  better test.
- Not done from the plan: the two Metal-oriented D3D9 options (`deAliasedSamplers`, `renderTargetHazardCopies`)
  from the prior-art notes are not implemented. Look at them if rendering shows wrong textures or feedback loops.
- Run: `ME_PC_DXVK=$PWD/work/dxvk-mac/dxvk-3.1.1-git/build-macos/src/d3d9/libdxvk_d3d9.0.dylib work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user`
  (add `DXVK_LOG_LEVEL=info DXVK_LOG_PATH=<dir>` for DXVK's own log).

## Phase 5 findings (end of session 1, 2026-11-06)

**The window was black although the game rendered. Cause and fix.** The backbuffer dump (below) showed a lit, real
image, so the picture was lost between DXVK and the screen. SDL's source (`src/video/cocoa/SDL_cocoavulkan.m`, SDL 3.4.18)
shows `SDL_Vulkan_CreateSurface` creates a new Metal view and `CAMetalLayer` *inside the call*. DXVK makes that call from
the game's worker thread; Cocoa wants it on the main thread, and a layer made elsewhere never shows. Fix: in DXVK's
SDL3 back end (`wsi_window_sdl3.cpp`, `createSurface`) hop to the main thread with `SDL_RunOnMainThread` when
`!SDL_IsMainThread()` (added `SDL_IsMainThread`/`SDL_RunOnMainThread` to `wsi_platform_sdl3_funcs.h`). This works
because `rt_host_macos.c` keeps pumping SDL events on the main thread. In `pc/dxvk/dxvk-3.1.1-macos.patch`.
After the fix the user saw the picture, with keyboard and mouse responding (the DirectInput mouse log shows relative
motion arriving).

**Tool added: backbuffer dump.** `ME_PC_DUMP_FRAMES=300,900,1500 ME_PC_DUMP_DIR=<dir>` writes `frame_<n>.ppm` from the
D3D9 backbuffer just before the n-th Present and logs the share of non-black pixels and the mean RGB. It reads
the game's own render, independent of presentation, so it separates "the game draws wrong" from "the window shows
wrong". Convert to PNG with a few lines of Python (zlib) to look at it. (`rt_d3d9.c`, `d3d_stat` gets the device via the
generator's `COUNTED` hook.) Frame numbers count Presents since start; the engine startup phase (1 draw/frame, 30 fps,
a loading screen or movie) lasts a variable 500-1800 frames, then ~60 fps with ~90-110 draws/frame.

**What the main menu frame looks like (backbuffer, 960x600, A8R8G8B8):**
- Layout is right: three red vertical bars, a red panel on the right with menu rows, controller glyphs (X, Y, A)
  at the bottom, a recognisable overexposed white city skyline. UI reds are red, so a plain R/B swap of the whole
  frame is *not* the problem in the backbuffer.
- **Every text label is a solid white or black rectangle**; the controller glyphs (which are probably a different
  texture) are fine. Text during startup (frame 800) showed as red blocks. So the font texture or its alpha is sampled
  wrongly (candidates: a texture format the D3D9 -> Vulkan mapping gets wrong on MoltenVK, e.g. A8, L8, A8L8, DXT3/5
  alpha, or a missing swizzle; or the font atlas not being uploaded).
- The sky and distant buildings are washed out and flat-banded (yellowish gray), which looks like missing or wrong
  textures/lightmaps and/or missing tone mapping rather than a final-image problem.
- The user reported "garbled colours" in the *window*; the dump looks colour-plausible, so part of what the user sees may
  come from the present step (a format/colour-space/scaling difference between the 960x600 backbuffer and the 640x360
  swapchain) and part from the content problems above. **Not yet compared side by side.**

## Phase 5 findings, session 2 (2026-11-06): text boxes and colours fixed

**Root cause (measured).** Every texture in the game was sampled as `(r, r, r, r)`. DXVK 3.1's SM3 lowering
(`subprojects/dxbc-spirv/sm3/sm3_resources.cpp`, `emitSampleColorOrDref`) samples each D3D9 sampler through one image
variable with two run-time paths chosen by a spec constant: a normal sample and a depth-compare (`Dref`) sample for
shadow maps. The SPIR-V declares the image as a colour image (`OpTypeImage ... 2D 0 ...`), but SPIRV-Cross's MSL back
end types any image that is used with `Dref` as `depth2d<float>`. Metal's `depth2d::sample` returns one float, which
the shader splats to four components. Measured with `MVK_CONFIG_SHADER_DUMP_DIR` (MoltenVK's MSL) and
`DXVK_SHADER_DUMP_PATH` (DXVK's SPIR-V): 14 of 18 fragment shaders of a menu run had `depth2d` textures, including the
UI shader that draws text.
- Text: UE3/Scaleform font pages are DXT5 with white RGB and the glyphs in alpha; alpha read as red (1) made every glyph
  quad opaque in the vertex colour: white or black boxes.
- 3D backdrop: diffuse textures read as their red channel, i.e. grey; that is the washed-out, flat, yellowish look.
- Not the cause (measured): the bridge's texture uploads (data arrives; earlier session), sRGB (forcing
  `SRGBTEXTURE=0` changed nothing about the boxes), bound textures and states (the per-draw trace shows the right
  textures, formats and blend states on every draw).

**Fix (our own code, in `pc/dxvk/dxvk-3.1.1-macos.patch`).** New D3D9 option `d3d9.emulateDepthCompare` (auto: on
when the Vulkan driver is MoltenVK; DXVK logs "D3D9: Emulating depth-compare sampling in shaders."). The
depth-compare branch no longer uses a `Dref` instruction: for 2D it gathers the 2x2 depth footprint, compares each
texel in the shader (`LESS_OR_EQUAL`, the compare op DXVK gives D3D9 shadow samplers) and weights the results
bilinearly (percentage-closer filtering as a linear compare sampler does); cube maps and samples with explicit
LOD/gradients do one sample and compare. The sampler gets compare off. The option is part of `D3D9ShaderOptions`, so
it changes the shader key and old entries in DXVK's shader cache are not reused.
- Result (measured, backbuffer dumps, same frame and input timing): the "Press Any Key" title and the main menu
  render as expected: readable text, coloured controller glyphs, the logo, a white/blue city with reflections. After
  the fix 0 of 36 fragment shaders in a run are typed `depth2d`. No MoltenVK or DXVK errors in the logs.
- **Not yet verified:** the emulated compare itself. The menu samples no depth textures (trace: only DXT1/DXT5, L8,
  A8R8G8B8, A16B16G16R16F, A32B32G32R32F, G16R16F), so shadow maps are first exercised in a level. Watch for shadow
  acne/edges and for point-filtered shadow maps (the gather path always filters bilinearly).
- The colours look right by eye; there is no Windows reference image to compare against, so "colours correct" is
  an impression, not a measurement. Tone mapping and the float render targets (A16B16G16R16F scene target, then a
  post-process into the A8R8G8B8 backbuffer) work.
- The two Metal-oriented options from the prior-art notes (`deAliasedSamplers`, `renderTargetHazardCopies`) were not
  needed for this.

**To reproduce (frame numbers are Presents since start; the title screen is up from about frame 900).**
```sh
# DXVK with the patch: build a patched copy (see pc/spikes/macos-dxvk/README.md), e.g. in work/dxvk-textfix/
export ME_PC_DXVK=$PWD/work/dxvk-defer/bin/libdxvk_d3d9.0.dylib
ME_PC_DUMP_FRAMES=1800 ME_PC_DUMP_DIR=work/frames-x ME_PC_AUTO_KEYS=32 ME_PC_ASSUME_FOCUS=1 \
  work/pc-recomp/bin-macos/me_pc work/pc/Binaries work/pc-deps work/pc-user
```
`ME_PC_AUTO_KEYS=32` taps Return 32 s after start to leave the title screen; frame 1800 is then the main menu. With
the unpatched DXVK the same command gives the boxes and grey colours (session 2: `work/frames-textfix/t8/frame_1800`
before, `t7/frame_1800` after; game art, never commit).

**Tools added.** `ME_PC_TRACE_FRAME=<n>` logs every draw of frame n: primitive, shader versions/sizes/hashes, render
target, blend/alpha/sRGB states, each bound texture with type, format, size, pool and sampler states
(`ME_PC_TRACE_MAX` caps the count, default 400). `ME_PC_TRACE_DUMP=1` also writes shader bytecode (`ps_/vs_<hash>.bin`)
and lockable 8-bit textures into `ME_PC_DUMP_DIR`. `ME_PC_AUTO_KEYS=<s>,<s>...` taps Return at those seconds.

## Phase 5 findings, session 2 (continued): play test in the tutorial

The tutorial level loads, renders (textures, lighting, sky, HUD) and plays at a steady 60 fps. Found and fixed during
the play test (each measured, commits on `prasys/text-colour-debug`):
- **Crash in the menu:** `missing import netapi32.dll!Netbios` (the game looks for network adapters after `socket()`
  fails). Stub reports no adapters (`eae5bca`).
- **Mouse counted twice** while captured: relative motion plus deltas of the `WM_MOUSEMOVE` positions the host also
  posts, which are clamped to the window. Captured relative motion now replaces them (`7701555`). Scripted test: +400/-400
  and -150/+150 counts reach the game exactly (DirectInput log) and the view turns and returns (within ~2 degrees: the
  game's own mouse smoothing).
- **Tearing and judder when walking/turning:** the game asks for `D3DPRESENT_INTERVAL_IMMEDIATE` (`UseVsync=False`) and
  paces itself with a 62 fps limiter; on the 120 Hz ProMotion screen that gave frames of 20-34 ms nearly every second
  (scripted 10 s pan). The bridge now asks for interval 1 on macOS (`ME_PC_VSYNC=0` to disable): no frames over 25 ms
  (`bd47b88`). The user's `TdEngine.ini` was also set to `MaxSmoothedFrameRate=60` (was 62; backup
  `TdEngine.ini.bak-62fps` next to it in `work/pc-user`) so each frame is exactly two refreshes.
- **Sampler heap not bound on MoltenVK:** Metal validation showed shaders reading the 16 KB sampler heap (set 0) from a
  160-byte buffer. The DXVK patch re-binds the sampler set with every resource-set bind on MoltenVK; the validation
  error is gone (`0a24f91`). This did **not** stop the lost-device freeze below.

**Still open (next session starts here)**

1. **Fixed in session 3 (2026-11-07): deferred destroy (margin 200) in `pc/dxvk/dxvk-3.1.1-macos.patch`; 0 of 5
   unattended level loads and one user play session lost the device (see "Phase 5, session 3" and
   `docs/todo/MARGINS.md`).** Previous state: **GPU device lost during level loads (freezes the game; blocks testing).** **Session 3: root cause found. The
   current state, measurements and next steps are in `docs/todo/MARGINS.md` (start there); the text below is the
   session-2 state.** In about a third to half of level loads
   (unattended repro: `ME_PC_AUTO_KEYS=32,36,40,44,48`, dies near frame 2650-2830), MoltenVK reports
   `MTLCommandBuffer execution failed (code 9): Invalid Resource` and every later submit fails with
   `VK_ERROR_DEVICE_LOST`; the game keeps counting frames but the window freezes. With `MTL_DEBUG_LAYER=1
   MTL_DEBUG_LAYER_ERROR_MODE=assert` it aborts in DXVK's `dxvk-submit` thread (`vkQueueSubmit2` ->
   `MVKQueueCommandBufferSubmission::commitActiveMTLCommandBuffer`): "command buffer references deallocated object";
   with `nslog` mode the object is a **1280x720 RGBA16Float texture** (the game's scene render target,
   A16B16G16R16F) "destroyed while still required to be alive by the command buffer". Measured: not caused by the
   depth-compare patch (stock DXVK fails too), not the shader cache (fails with and without), not DXVK's memory
   defragmentation (`DXVK_CONFIG="dxvk.enableMemoryDefrag = False"`: 1 of 3 runs still failed). Next: log every D3D9
   texture/surface `Release` with refcounts around the level start and check the bridge's render-target surface
   proxies (`rt_d3d9.c` `d3d_wrap`/`d3d_forget`: surfaces of a texture keep their proxies after the texture dies,
   and DXVK can reuse the address); then look at MoltenVK deferring Metal encoding to submit time.
2. **Fixed by the upstream merge (session 3, 2026-11-07; observed by the user in play, not measured with dumps): see "Phase 5, session 3: upstream merge".** Previous state: **One-frame wrong-camera flashes ("POV jumps", Faith looks unnatural).** Caught with consecutive dumps while
   walking (`work/frames-textfix/w2/frame_3150..3157`): seven frames look at the roof tiles, frame 3154 alone is
   drawn from another camera (a view over the city like the tutorial's start). Guest memory ordering is already x86
   TSO (`X86_TSO=1` in `x86rt.h`: acquire loads, release stores), so a plain weak-memory race is unlikely. Possibly the
   same render-target lifetime problem as item 1. Next: dense capture `ME_PC_DUMP_FRAMES=2990-3400/2` while walking
   (`ME_PC_AUTO_INPUT=3000:down=W;3400:up=W`), count wrong-camera frames, and correlate with render-target changes
   (`ME_PC_TRACE_FRAME` on a bad frame).
3. *Clipping* (the user reports it gone with the upstream merge, like item 2; not re-checked with dumps) reported by the user (geometry cut open near the camera, popping). Not yet reproduced in a dump; may be
   item 2. Depth clip is right in principle (MoltenVK maps `vkCmdSetDepthClipEnableEXT(true)` to
   `MTLDepthClipModeClip`, read from MoltenVK 1.4.2 source). Occlusion queries: `ME_PC_LOG_QUERIES=1` exists, not yet
   read in a level.
4. *Shader precompile* (GitHub issue prasys/RunnerRecompiled#1; session 3 measured a 23.4 s first frame of the
   tutorial with a cold cache, 1.3-3.9 s with warm caches, and ~1 s hitches in the level). The first frames of a level take ~1-2 s (pipelines built on first draw; MoltenVK has no
   pipeline libraries). DXVK's own shader cache works (`DXVK_SHADER_CACHE_PATH`); Metal pipelines are not cached.
   **Done (session 5, 2026-11-07): pipeline warm-up in the DXVK patch.** Without pipeline libraries the first draw
   with a new state compiles synchronously. `DxvkGraphicsPipeline::getPipelineHandle` now times and logs every such
   compile (`Pipeline compiled on demand: N ms`, only >= 2 ms) and records (VS name, FS name, raw
   `DxvkGraphicsPipelineStateInfo`) to `~/.cache/dxvk/pipelines.warm` (`DXVK_PIPELINE_WARM_FILE` overrides,
   `DXVK_PIPELINE_WARM=0` disables). On the next run `DxvkPipelineManager::registerShader` queues the recorded states on
   the compiler threads as soon as both shaders of a pair exist (the game creates them while loading). Measured on the
   tutorial load, same machine, unattended: 321 on-demand compiles / 1280 ms / one 545 ms frame with an empty file,
   then 85 / 439 ms (one 122 ms outlier, the rest <= 6 ms) with 352 recorded states, and no frame over 42 ms at the
   level start. The first run ever still pays full price (the file fills as you play; one tutorial load is enough for
   that level). **Present wait (same day):** `D3D9SwapChainEx::Present` calls `DxvkPipelineManager::waitForWarmup()`, which sleeps while
   compiler tasks are queued (only if warm states were replayed; cap `DXVK_PIPELINE_WARM_WAIT_MS`, default 30000, 0 = off;
   logs `held present N ms for M tasks`). Measured with the 352-state file: on-demand compiles 85 -> 3 (9 ms total), about
   3 s held across the level load, level at 60 fps with no frame over 19 ms. Not done: shipping a pre-recorded file
   (plan: play every chapter/checkpoint once with the default file, then commit it and seed `~/.cache/dxvk` from it);
   **Recording status and open problems (session 5, end).** `work/record-warm.sh` records interactively (timestamps on the
   DXVK log, and on the runtime log with `ME_PC_LOG_FILES=1`); the checklist of maps/sections is
   `docs/todo/WARMUP-CHECKLIST.md`. After the user's play (Prologue, Chapter 1 start, part of Chapter 2) and a Codex menu
   probe the warm file holds about 1,024 states (502 KB, in `work/warm-record/`, not committed: not complete yet).
   **Controlled comparison (session 5, 1,024 recorded states, 8 identical scripted runs of ~4,700 frames, `work/mvk-test/run.sh`):**
   warm-up on: 3-4 compiles during play, present held 1.4-1.5 s in total; warm-up off: 336 compiles during play; cold vs
   warm DXVK shader cache: no difference (1.1 s vs 1.4 s held, so a cold shader cache is not what caused the 19-30 s
   holds); lost device 0 of 8 either way (too few events to tie it to warm-up); worst frame ~1020 ms in every run, also
   with warm-up off, so that single 1 s frame is **not** a pipeline compile (it is the "single ~1 s frames" item).
   `MVK_CONFIG_TRACE_VULKAN_CALLS=6` on 406 `vkCreateGraphicsPipelines` calls: median 53 ms, p90 205 ms, max 308 ms,
   32.5 s of compile time in 100 s; DXVK makes no separate `vkCreateShaderModule` call, so MoltenVK's SPIR-V to MSL
   conversion is inside that call and the conversion/Metal split is unknown. `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1`
   was inconclusive (the runs went through different content). DXVK passes `VK_NULL_HANDLE` as the pipeline cache, so
   nothing is persisted on the MoltenVK side; a persisted `VkPipelineCache` would only save the MSL conversion (MoltenVK
   user guide, "Shader Loading Time"), not Metal's own compile. The 19-30 s holds are unexplained: they happened only in
   Codex runs that went into other chapters (SP02, SP09) and did not reproduce.
   **The periodic ~1.02 s frame found (session 6, 2026-11-08):** `ME_PC_LOG_WAITS=<ms>` logs guest waits/sleeps of at least that
   long with the guest caller. The 1 s frames are the engine's own `appSleep(1.0f)` (`f_010ED040` -> `Sleep(1000)`), called at
   `0x11C6476` inside `f_011C6360`, right after `f_00B30E60` and before the rendering thread is recreated (the `CreateThread ...
   RenderingThread` line follows each one). `rt_kernel32.c` `k_Sleep` now skips exactly that call site (`ME_PC_KEEP_SLEEP1000=1`
   keeps it): in an 80 s run the worst frame after the level start fell from ~1020 ms (every ~12 s) to 28 ms. Not yet checked
   for a race on a long play. The remaining ~1 s frame at the level start is the burst of ~340 pipeline compiles (1.3 s, empty warm file).
   **Map-tagged warm-up with early shader creation (session 6, 2026-11-08, issue #9).** The warm file is v2 (each record tagged with
   the map; the old file is kept as `.old`). The runtime tells DXVK the map as soon as its persistent package opens
   (`*_p.umap`, `*_p_LOC_int.upk`, `TdMainMenu`) through `dxvk_warm_set_map`. DXVK replays only that map's states, and the bytecode
   of every recorded shader is saved in `pipelines.warm.shaders`; at the map name a helper thread re-creates those shaders through
   the D3D9 `CreateShaderModule` path, which registers them and queues their pipelines while the game is still loading (the
   game's own `CreateVertexShader`/`CreatePixelShader` then hit the same module). Measured, edge_p, one run each: first scene interval
   6 (early) vs 8 (replay without early creation) vs 7 (no warm-up, 435 on-demand compiles / 1.6 s in play); 1 on-demand
   compile in play. Knobs: `DXVK_PIPELINE_WARM_EARLY=0`, `DXVK_PIPELINE_WARM_HOLD_MIN`, `DXVK_PIPELINE_WARM_HOLD_DRAWS`. The remaining hold is
   the compile throughput (~50 tasks/s on 10 cores), not threads. A map has to be played once with a v2 file to be recorded.
   Two things went wrong: (1) **the lost GPU device came back** in a 4-minute session (Prologue part 2 load,
   `VK_ERROR_DEVICE_LOST`, no Metal error captured because the MoltenVK log level was off; `record-warm.sh` now sets
   `MVK_CONFIG_LOG_LEVEL=2`); later sessions did not lose it. (2) **With 1,024 states the present wait hit 19 s and the
   30 s cap in two Codex runs (60 and 147 tasks pending)**: some compile tasks are slow or stuck, or the queue is
   dominated by states for other chapters. Not diagnosed. The default cap is now 10 s so a level load cannot freeze for
   long; next: log per-task compile time, and replay only states whose shaders are used by the loaded map. Unattended
   chapter loading by scripted keys did **not** work (two agents tried, DeepSeek and Codex): the saved profile goes
   straight into a story flow (SP09 or SP02 assets), Backspace shows the Story submenu with "Continue Game" and "Play
   Chapter" below it; Down, Return is the likely path but unverified. Recording the other chapters stays manual for now.
   other levels. A cold DXVK shader cache (`~/.cache/dxvk`) is a
   separate cost: one cold run sat in the level load for over two minutes at 85% CPU with only ~170 ms of pipeline
   compiles logged, so the stall is **not only pipelines**; not profiled (`sample` wrote no file).
5. *Shadows* (first real test of the emulated depth compare) and the Switch Pro / Xbox controller's right stick are
   not yet checked by eye.
6. *Upstream report.* The `depth2d` typing is a DXVK-on-MoltenVK interaction (SPIRV-Cross behaviour plus DXVK 3.1's
   one-image-two-paths lowering); other macOS DXVK users will hit it. Worth reporting with the MSL excerpt.
7. *Window vs dump.* Dump the swapchain image or compare screenshots by the user with `frame_*.png`.
8. *Process death.* (Session 2: the user's exit-137 was a force-quit of the frozen game from item 1.) Two runs with frame dumping on ended by themselves at 62-72 s with **exit code 137 (SIGKILL)**,
   no `FATAL`, no signal message, no macOS log entry, 32 GB RAM with no pressure. The runtime's own exits are `exit(3)`
   (`rt_fatal`) and `ExitProcess`, so this was something else. A five-minute run without frame dumping ended with code 1
   when the user closed it. Unexplained. Next: check `~/Library/Logs/DiagnosticReports`, sample RSS during a run, and run
   once without dumping *and* once with `DXVK_LOG_LEVEL` unset; also check whether it is the tool environment killing
   background jobs rather than the game.
9. *Input/audio details.* Keyboard and mouse respond. Not yet checked: the Switch Pro Controller in the menu, audio
   quality, the startup movie, entering the tutorial level, and PhysX under play.

## Phase 5, session 4: PhysX effects on Shatter (2026-11-07, issue #2)

**Problem.** With `PhysXEnhanced=True` the lifted PhysX core ran the effects slowly: unattended tutorial run (150 s, one
run each) 24 of 83 in-level seconds below 50 fps, 16 below 25 fps; effects off: 2 of 114.

**What was done.** [Shatter](https://github.com/Arecsu/shatter) v1.0.7 (MIT) replaces the game's `NxFluid` and `NxCloth`
with multithreaded CPU solvers and leaves rigid bodies on the core. Its solvers and hosting code are vendored in
`pc/shatter/` and run natively on arm64 (SSE through sse2neon; results bit-identical to SSE2 on 13 upstream scenarios).
`pc/shatter/port/` is the guest/host bridge: guest thunks for host methods (`SHATTER_THUNK`), guest `__thiscall` calls
into the lifted core (`guestThiscall`, out-parameters through guest stack scratch), and the hooks: the runtime's new
export hook (`rt_set_export_hook`) wraps PhysXLoader's `NxCreatePhysicsSDK`, which patches the core's `NxPhysicsSDK` and
`NxScene` vtables in guest memory (lifted code reads vtables from memory, so patched slots dispatch to the thunks). Fluid
and cloth objects live in guest memory with thunk vtables. Details and the list of changes to upstream:
`pc/shatter/UPSTREAM.md`.

**Measured (M1 Max, one interactive session each, same levels, effects on).** Cloth on the lifted core, fluids native:
20% of in-level seconds below 50 fps, 175 frames over 25 ms per minute, lifted core up to 16 ms per frame (600-frame
average; peak 94 ms). Both native: 9%, 41 per minute, lifted core at most 0.6 ms (peak 31 ms). Native cloth: 59 cloths
(10 to 1156 vertices), 0.17 ms per frame on the cloth thread; fluids 0.07 to 0.2 ms per frame on the game thread.
Not measured: visual correctness against Windows (user play only).

**What remains** is not physics: single frames of about 1.0 s (about a dozen per session) and the level-start stall
(issue #1). Not ported from Shatter: the slide-steering fix above 62 fps (it patches exe code bytes; would need a hook in
the lifted function), `shatter.ini` (environment switches only), and upstream's debug tools (pass-through call counters,
fluid trace, cloth recordings).

## Phase 5, session 3: upstream merge (2026-11-07)

Merged `origin/main` (`singlemate/RunnerRecompiled`, 4 commits up to `4c247be`) into `prasys/text-colour-debug`.

**What came in and why it matters on macOS.**
- `82dd744` EFLAGS across calls (`pc/tools/xflags.py`, `X86Ctx.flags`): the hand-written msvcr80 math passes ZF through
  call/ret and lifted code kept flags in a local, so `sin/cos/tan/asin/acos/atan` could take the special-value branch
  and return garbage (wrong view pitch, poses in jump/crouch). Shared lifter code: our build had the same bug. Also
  unordered stack loads/stores (`lds*/sts*`) in `lift.py`. On macOS the lift reports flag-passing functions in 5
  modules (largest: 12 live-in, 3 exports); the other 9 are unchanged.
- `d2fd868`: `x86_dispatch` takes the thread from `ctx` (no TLS read). The `-march=armv8.4-a` change is Android only:
  llvm@23 clang for `-arch arm64` already emits `ldapr` for acquire loads (checked on a test file).
- `4c247be`: DXVK took pool allocations without `m_mutex` on the import paths (`dxvk_memory.cpp`); the lock is now in
  `pc/dxvk/dxvk-3.1.1-macos.patch` too. `PostThreadMessageW` to another thread: we had made the same fix; upstream's
  names (`rt_thread_host_id`) were kept, with our logging.
- `148f03d`: `host_wait_window()` before `CreateDevice`/`Reset`; a no-op in `rt_host_macos.c`.

**Measured.** Build `work/pc-recomp-upstream` (`--regen`, 429 s, 0 errors). `ME_PC_CRT_TEST=1`: 176 of 176 CRT math
checks match libm. Unattended tutorial runs (`ME_PC_AUTO_KEYS=32,36,40,44,48`, 180 s each): the lost device (item 1)
is still there (2 of 3 runs lost the device, near frames 1497 and 2345; run 3 reached the level at 60 fps); nothing in the merge addresses it.

**Lost device fixed (session 3, later).** The deferred-destroy change from `docs/todo/MARGINS.md` was applied on top
of the merged build: 0 of 5 unattended 150 s tutorial runs and one user play session (about 15,000 frames, Quit Game
from the menu, exit code 0) lost the device. It is now in `pc/dxvk/dxvk-3.1.1-macos.patch` as DXVK options
(`dxvk.deferResourceDestroy`, Auto = on for MoltenVK; `dxvk.deferResourceDestroyMargin`, default 200); the test-only env
var and debug logging were removed. Built in `work/dxvk-defer/` (dylib copy in `work/dxvk-defer/bin/`). A second upstream
merge brought `f9cd798` (achievements hook, `ME_PC_ACH_PATH`; Android launcher), merged with `-X renormalize` after the
repository moved to LF line endings (`.gitattributes`).

**Observed by the user in play (not measured):** the one-frame wrong-camera flashes (item 2) and the other visual
problems reported in session 2 (clipping, Faith's poses) are gone. Reading (an **assumption**): garbage trig results
from the CRT gave a wrong camera matrix on some frames. Not confirmed with dumps or `ME_PC_FRAME_TRACE`.

## Prior art (discovery agent, 2026-11-06; claims not yet re-checked by hand)

Ideas only. Rule: write our own code and credit ideas; do not copy code from projects without a permissive license.

| Project | License | Relevance |
|---|---|---|
| TalhaMudo/NFSU2Recomp | all rights reserved | Lifted x86-32 D3D9 game on macOS arm64 with its own D3D9-to-Metal renderer. Memory: 4 GB `PROT_NONE`, commit in 16 KB units with a bitmap, SIGSEGV+SIGBUS in one handler, `sigaltstack` per thread, PC from `__ss.__pc`. SEH is stubbed and x87 uses doubles. |
| daniiichan/Need-for-Speed-Underground-2-macOS | none detected | DXVK Native + MoltenVK + SDL2 on macOS. 4 KB guest page protection is checked **in software** (page map) instead of by hardware `mprotect`. macOS fixes: create `CAMetalLayer` on the main thread, duplicate sampler bindings in the shader compiler, swapchain-blit culling. |
| sp00nznet/pcrecomp | MIT | Reserves in 64 KB units; decommit re-`mmap(MAP_FIXED)` for interior pages (fresh zero pages) and `memset` for edges; `_ftol`/`_CIpow` against the lifted FPU stack; a guest-stack-walking C++ unwinder exists for 64-bit only (32-bit guest SEH out of scope). |
| rexglue-sdk | BSD-style headers, mixed | `exception_handler_posix.cpp` handles SIGBUS on macOS and reads `__ss.__pc`. `memory_posix.cpp` decommits with `MADV_DONTNEED`, which does not zero on Darwin: do not copy. |
| metalsharp/DXVK-MacOS, Gcenx/DXVK-macOS | zlib | Stock MoltenVK is not enough for DXVK 3.1: they use a patched MoltenVK/SPIRV-Cross and two D3D9 options for Metal (`d3d9.deAliasedSamplers`: one binding per texture type per sampler slot; `d3d9.renderTargetHazardCopies`: sample a copy of a bound render target). Needs Vulkan 1.3 incl. maintenance5/6 and `VK_KHR_portability_enumeration` on the instance. |
| PhysX 2.8 replacement | none found | Open PhysX is 4.x/5.x and not API-compatible. Lifting the game's own `PhysXCore.dll` stays the plan for rigid bodies. |
| Arecsu/shatter | MIT | Not a PhysX replacement: replaces only the enhanced effects (`NxFluid`, `NxCloth`) of the game's PhysX 2.8 with multithreaded CPU solvers, rigid bodies stay on the core. Ported in session 4 (`pc/shatter/`). |

Decisions taken from this:
1. Memory: reserve 4 GB `PROT_NONE`; commit in whole 16 KB host pages; zero by re-`mmap(MAP_FIXED|MAP_ANON)`; never rely on
   `MADV_DONTNEED`. Keep per-4 KB guest protection in a software page table if the game needs it (see open question).
2. One fault handler for SIGSEGV and SIGBUS, on a per-thread `sigaltstack`.
3. Guest exceptions (SEH, C++): not solved by any macOS project we found. Our runtime has its own implementation
   (`pc/tools/eh.py`); stress it early in bring-up.
4. Keep x87 as `double` by default (as Android), but leave room for an 80-bit mode.
5. Phase 4 starts from DXVK 3.1.1 Native with our own re-implementation of the two Metal-oriented D3D9 options.

## Risks, most likely first

1. DXVK on MoltenVK lacks something the game's shaders need (D3D9 needs far less than D3D11; unverified).
2. Apple Silicon 16 KB pages versus 4 KB guest pages and the 4 GB reservation.
3. Compile time and memory for about 5.7 million lifted instructions of C.
4. x87 modelled as `double` (same as Android).
5. Faults arrive as `SIGBUS` as well as `SIGSEGV`; thread-state layout differs from Linux.

## Open questions

- **PhysXCooking.dll**: `build_pc_runtime.py` lists it, but PhysX 2.8.0 names the cooking library `NxCooking.dll`, which
  the game ships. `PhysXLoader.dll` contains the string "PhysXCooking.dll". The only `PhysXCooking.dll` on hand is
  2.8.3 (in the 9.17 System Software). Make the module optional and see whether the runtime ever asks for it.
- **PhysX replacement**: open-source PhysX 3/4/5 is not API-compatible with 2.8; see `DECISIONS.md`. Rigid bodies stay on
  the lifted core (under 1 ms per frame measured); the effects moved to Shatter in session 4.
