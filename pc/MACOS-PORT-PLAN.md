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
| 4 | DXVK 3.1.1 D3D9-only for macOS arm64 on MoltenVK, SDL3 WSI, `dlopen("libdxvk_d3d9.dylib")` | **the game renders on it: main menu at ~60 fps** (2026-11-06; picture not yet inspected). See "Phase 4 results" |
| 5 | Bring-up: build full lifted game, title, menus, movies, sound, first level, PhysX | open |
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
  main menu. That is evidence of rendering, not of correct rendering: nobody has looked at the picture yet.
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

## Prior art (discovery agent, 2026-11-06; claims not yet re-checked by hand)

Ideas only. Rule: write our own code and credit ideas; do not copy code from projects without a permissive license.

| Project | License | Relevance |
|---|---|---|
| TalhaMudo/NFSU2Recomp | all rights reserved | Lifted x86-32 D3D9 game on macOS arm64 with its own D3D9-to-Metal renderer. Memory: 4 GB `PROT_NONE`, commit in 16 KB units with a bitmap, SIGSEGV+SIGBUS in one handler, `sigaltstack` per thread, PC from `__ss.__pc`. SEH is stubbed and x87 uses doubles. |
| daniiichan/Need-for-Speed-Underground-2-macOS | none detected | DXVK Native + MoltenVK + SDL2 on macOS. 4 KB guest page protection is checked **in software** (page map) instead of by hardware `mprotect`. macOS fixes: create `CAMetalLayer` on the main thread, duplicate sampler bindings in the shader compiler, swapchain-blit culling. |
| sp00nznet/pcrecomp | MIT | Reserves in 64 KB units; decommit re-`mmap(MAP_FIXED)` for interior pages (fresh zero pages) and `memset` for edges; `_ftol`/`_CIpow` against the lifted FPU stack; a guest-stack-walking C++ unwinder exists for 64-bit only (32-bit guest SEH out of scope). |
| rexglue-sdk | BSD-style headers, mixed | `exception_handler_posix.cpp` handles SIGBUS on macOS and reads `__ss.__pc`. `memory_posix.cpp` decommits with `MADV_DONTNEED`, which does not zero on Darwin: do not copy. |
| metalsharp/DXVK-MacOS, Gcenx/DXVK-macOS | zlib | Stock MoltenVK is not enough for DXVK 3.1: they use a patched MoltenVK/SPIRV-Cross and two D3D9 options for Metal (`d3d9.deAliasedSamplers`: one binding per texture type per sampler slot; `d3d9.renderTargetHazardCopies`: sample a copy of a bound render target). Needs Vulkan 1.3 incl. maintenance5/6 and `VK_KHR_portability_enumeration` on the instance. |
| PhysX 2.8 replacement | none found | Open PhysX is 4.x/5.x and not API-compatible. Lifting the game's own `PhysXCore.dll` stays the plan. |

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
- **PhysX replacement**: open-source PhysX 3/4/5 is not API-compatible with 2.8; see `DECISIONS.md`. Revisit only after
  the lifted DLLs run.
