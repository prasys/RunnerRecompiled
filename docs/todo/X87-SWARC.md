# SwARc x87 model investigation

Date: 2026-11-15. SwARc inspected read-only at `work/swarc` (commit `6b25b3c`); no source copied.

## Recommendation

**Adopt only guarded**, in stages. Keep binary64 as a fast path only under proven conditions. A true exact path needs explicit 80-bit state and portable software arithmetic; `long double` is binary64 on arm64 macOS. The prototype here is a build-flagged seam, not an exact implementation, and must remain disabled.

## Current behavior and divergences

`X86Ctx` in `pc/runtime/x86rt.h` stores physical `st[8]` as `double`, with TOP, occupied-bit mask `ftag`, `fcw` and `fsw`. Push decrements TOP; pop empties the slot and writes negative quiet NaN indefinite. `finit` resets CW to `0x037f`, empties the stack and clears status. Status composition inserts TOP.

`lift.py` emits helper calls/expressions per x87 instruction. Arithmetic runs in host binary64, then `x87_round` rounds PC=24 to a 24-bit significand (while retaining double exponent); PC=53 and PC=64 both retain binary64. RC applies to `fist` and `frndint`, not arithmetic or m32/m64 stores. Integer conversion returns the indefinite integer for invalid range, but does not model exception flags. m32/m64 loads/stores use C conversions. `fld`/`fstp tbyte` go through `x87_ld80`/`x87_st80`, converting via binary64 and losing precision/range. Compare sets C0/C2/C3 (including unordered); `fxam` classification is simplified. `fnstsw`, `fnstsw ax`, `fldcw` and `fnstcw` are emitted. Trig uses libm and sets/clears C2 for the large argument case; `fyl2x` uses host `log2`, `fpatan` uses `atan2`; `fsqrt` uses host sqrt. `fprem` forms are unsupported and trap.

Divergences from real x87 include: PC=64 and some PC=53 results; double rounding at PC=24; binary64 exponent range; tword precision, exponent and special encodings; denormals; RC on arithmetic and float/double stores; sticky IE/DE/ZE/OE/UE/PE, exception masks, SF/ES/B and C1 rounding direction; invalid compare status; full C0/C2/C3 behavior; libm transcendental values/flags; fprem; and complete environment/tag-word save-restore semantics.

## SwARc approach and mapping

SwARc represents each value as explicit sign, 15-bit exponent and 64-bit significand, plus TOP, empty bits, CW and SW. Berkeley SoftFloat 3e `extF80` provides arithmetic/rounding; custom routines handle transcendental instructions. Guarded host-double fast paths accept only proven normal/zero inputs and bounded results, with error and flag checks; boundaries, specials, denormals, under/overflow and division by zero fall back to software arithmetic. Specialized cases cover PC=24/53 arithmetic, m32/m64 loads/stores and compare.

The lifter groups eligible straight-line instructions within a basic block into runs, maps symbolic stack positions to locals, guards once, and writes state back once. Our C emitter could use C ABI `x87_*` helpers with explicit register structs in `X86Ctx`, then add guarded runs to `lift.py`. It cannot directly consume SwARc's C++ API; context writeback and SEH boundaries matter. SwARc is MIT; SoftFloat is BSD-3. Copying code requires MIT attribution and NOTICE, plus SoftFloat notice if copied.

## Measurements

**Measured**, Apple arm64, clang `-O3 -ffp-contract=off`: `work/x87-swarc/bench.c`, 20M mixed add/mul/div/sqrt/conversion/compare loop per setting: PC=53 3.26 ns/op; PC=24 (including current significand rounding helper) 1.51 ns/op; PC=64 1.55 ns/op. This is only a host-double microbenchmark, not exact-vs-SoftFloat; it does not isolate fld/fstp, fcom or fistp and loop optimization may affect results.

**Measured from the static catalog, not a runtime profile:** `pc-recomp-main/mnemonics.json` has 5,733,692 instructions; the 52 mnemonics beginning with `f` total 170,643 (2.98%). Frequent forms include `fld` 48,503, `fstp` 43,382, `fadd` 13,347, `fmul` 9,999, `fxch` 8,268, `fst` 7,061, `faddp` 5,185, and `fnstsw` 2,830. This catalog counts all lifted game code and says nothing about hot paths. No `pc-profile.bin` exists, and the macOS runtime does not currently expose the Windows sampling profiler, so dynamic hot-path x87 counts remain unknown.

**Measured after syncing the macOS tree:** a full macOS arm64 runtime build completed in 398 s into the isolated `work/x87-swarc-build` directory, with the prototype build flag enabled. `PhysXCooking.dll` was absent and correctly skipped by the build script. A bounded 30-second run using the resulting binary reached the menu/rendering loop, logged up to 62 fps and exited cleanly (`guest ExitProcess(0)`). Its log/output and fresh DXVK caches were directed to `work/x87-swarc-run`; `ME_PC_DXVK` pointed at the existing `dxvk-main` build. No files under `~/.cache/dxvk` were intentionally used or modified.

The SwARc standalone tests remain unbuilt: CMake is unavailable, and `x87.cpp` requires Berkeley SoftFloat sources fetched by its CMake configuration. No random differential results are claimed. A real-x87 oracle is unavailable on arm64.

## Prototype and diff

Added a compile-time `X86_X87_EXACT` branch in `x87_round()` and a `--x87-exact` option to `build_pc_runtime.py`; both are OFF by default. The branch documents PC=64's binary64 limitation, and currently preserves legacy numerical behavior. The enabled arm64 build proves the gated branch compiles and runs in the port; it does not implement tword fidelity or full flags and is not a validated exact x87 model.

Diff: the worker's prototype (an inert `X86_X87_EXACT` branch in `x87_round()` and a `--x87-exact` option) is NOT merged; only this report is kept. Benchmark: `work/x87-swarc/bench.c` (not in the repo). No third-party code copied; no NOTICE entry required.
