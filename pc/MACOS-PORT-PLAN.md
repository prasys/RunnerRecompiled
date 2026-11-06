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
| 1 | Lifter and build scripts run on macOS (`libLLVM.dylib`, native arm64 clang) | **catalog done**, lifting/compile in progress (see "Phase 1 results") |
| 2 | Spike: 4 GB guest reservation, 16 KB pages, `mprotect`/fault behaviour on macOS arm64 | **done 2026-11-06** (see "Phase 2 results") |
| 3 | `rt_host_macos.c`: memory, threads, SIGSEGV/SIGBUS handler, files, SDL3 window and input, OpenAL Soft | open |
| 4 | DXVK 3.1.1 D3D9-only for macOS arm64 on MoltenVK, SDL3 WSI, `dlopen("libdxvk_d3d9.dylib")` | open |
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
