# Shader translator differential test: SwARc vs DXVK on Mirror's Edge's shaders (2026-11-15)

Question: does SwARc's D3D9 bytecode -> GLSL translator (`hlsl::TranslateBytecode`, MIT, commit `6b25b3c`) compute
the same results as the path the game uses today (DXVK 3.1.1: dxbc-spirv SM3 front end -> SPIR-V), on the game's
real shaders? Tools: `pc/tools/shader_diff/` (README there). Data, samples and logs: `work/shader-diff/` (game-derived,
not in the repo).

## Answer

**Yes for the arithmetic, with three small known gaps.** All 3,534 recorded shaders (3,386 pixel, 148 vertex, all
SM3) were run both ways on the GPU with identical inputs.

| | shaders | match, plausible inputs (`benign` + `signed`) | match, all four input sets |
|---|---|---|---|
| pixel | 3,386 | 3,380 | 3,280 |
| vertex | 148 | 148 | 138 |

Every one of the 116 differing shaders is explained (below), and none is a wrong translation of ordinary arithmetic:

- **106 shaders: a zero operand to `nrm`, `rsq` or `rcp`.** SwARc produces Inf/NaN, DXVK produces what the D3D9
  reference code produces. Three one-line fixes in SwARc's emitter; each of the 106 matches once its instruction is
  patched experimentally.
- **7 shaders: single pixels on a cube-map seam**, where last-bit differences pick another mip level. Not a
  translator difference.
- **3 shaders: overflow above ~1e37** (IEEE Inf/NaN vs DXVK's clamp to FLT_MAX). Not specified by D3D9.

Recommendation at the end.

## Method (measured on an M1 Max, macOS 15.8.1)

1. **DXVK's SPIR-V** came from one bounded game run with `DXVK_SHADER_DUMP_PATH` (current DXVK build
   `work/dxvk/bin`, scratch warm file; `~/.cache/dxvk` was listed before and after: unchanged). The launch-time
   rebuild created all 3,534 recorded shaders: 3,534 `.sm3_dxbc` + `.spv`. This is DXVK's real output, including its
   float emulation (`d3d9FloatEmulation` on) and D3D9 run-time state hooks. An offline tool was the alternative; DXVK
   3.1.1 has no separate `dxso` compiler any more (the SM3 front end is in `subprojects/dxbc-spirv`, the D3D9 lowering
   inside `d3d9_shader.cpp`), so the dump was less fragile.
2. **SwARc**: `translate.cpp` runs the translator and prepends a neutral prelude (vertex inputs as plain attributes,
   `r_position` = identity, alpha test passes, LOD bias 0, `r_vface` with DXVK's sign). 3,534/3,534 translate and
   compile with `glslangValidator`.
3. Both SPIR-V modules -> `spirv-cross --msl` (2.3). DXVK's unsized sampler heap is rewritten to a 1-element array
   first (`spvpatch.py`); nothing else in its SPIR-V is touched.
4. **Inputs** are a function of (shader, input set, register or semantic) only, so both sides get the same values:
   constant registers by number, varyings / vertex attributes by D3D9 semantic, textures by sampler register. The
   interfaces were cross-checked per shader: same varyings, vertex inputs, outputs and texture types on both sides for
   all 3,534.
5. **Execution** (`runner.swift`, Metal, fast math off for both): pixel shaders draw one triangle over an 8x8 viewport
   per input set (varyings interpolate, so `texld` gets real derivatives) into RGBA32F + Depth32F; vertex shaders are
   called from a compute kernel for 16 vertices per set, and position plus every `dcl`'d output (935 in total) is read
   back. 7,068 shader sides compiled and ran, 0 failures.
6. **Compare** (`compare.py`): match if `|a-b| <= 1e-4 + 1e-3 * max(|a|,|b|)`, or both NaN, or the same infinity; a
   pixel killed on one side only is its own class. Four input sets: `benign` (positive), `signed`, `edge` (zeros and
   ones mixed in), `zero` (all constants and inputs 0).

How close "match" is: on a sample of 470,123 finite pixel-shader components, 94.3% are bit-identical and 99.9% are
within 1e-5 relative; 0.09% differ by 1e-3 or more (the cases below).

The comparison is not vacuous: per input set about 2,860-2,890 pixel shaders produce varying, non-constant output;
498 have both killed and surviving pixels (so `texkill` agreement is tested per pixel); 12 kill every pixel in every
set and give no numeric evidence.

## What the game's shaders use

`python3 -I pc/tools/shader_diff/sm3.py <file>`. Pixel (3,386): `texld` 3,330, `nrm` 2,788, `dp2add` 2,092,
`exp`/`log` 1,989, cube samplers 1,691, `texkill` 569, `lrp` 515, `rcp` 509, `pow` 499, `cmp` 407, `frc` 359,
`rsq` 238, `vFace` 43, depth output 8, `texldp` 4, `ifc` 3, `rep` (with `defi`) 1, `sincos` 1, `texldl` 1. Vertex
(148): `mova` + relative constant addressing 38 (skinning), `exp` 38, `rcp`/`rsq`/`slt`/`sge` 31 each, `lrp` 29,
`log` 14, `sincos`/`nrm`/`frc` 8 each.

**Not in this game, so not tested at all:** `texldd`, `dsx`/`dsy`, `lit`, `dst`, `sgn`, `cnd`, `crs`, the `m4x4` family, `loop`/`call`/`setp`/predication, bool registers, `vPos`, MRT, SM1/SM2 shaders.
Tested by very few shaders: `texldl` (1), `texldp` (4), `rep` (1), `ifc` (3), pixel `sincos` (1); they match.

## Differences, by class

Counts are shaders; each differing shader differs in exactly one input set. "Attributed" means: the difference
disappears when only that instruction's zero case is patched in SwARc's GLSL (`--swarc-fix`, see the README).

| Class | pixel | vertex | input set | who is right |
|---|---|---|---|---|
| `nrm` of a zero vector | 54 | 8 | `zero` (3 in `edge`) | DXVK |
| `rsq` of 0 | 30 | 0 | `zero` | DXVK (see note) |
| `rcp` of 0 | 12 | 2 | `edge`, `zero` | DXVK for vertex shaders; unspecified for pixel shaders |
| cube-map seam / LOD (not arithmetic) | 7 | 0 | `benign` 1, `signed` 3, `edge` 3 | neither is wrong |
| overflow above ~1e37 | 3 | 0 | `signed` 2, `edge` 1 | unspecified |
| **total** | **106** | **10** | | |

### NaN / Inf handling (109 shaders)

- **`nrm` (62).** SwARc emits `v * inversesqrt(dot(v,v))`: for a zero vector that is `0 * Inf = NaN`. Microsoft's
  reference code for `nrm` is explicit: `if (f != 0) f = 1/sqrt(f); else f = FLT_MAX;` then `dest = src * f`, i.e. a
  zero vector stays zero. DXVK does exactly this (`min(rsqrt, FLT_MAX)`). **SwARc is wrong.** Seen as NaN in the
  output (49 + 8), as a different finite colour after the NaN went through a texture lookup (4), and once as a pixel
  DXVK kills and SwARc keeps (a NaN fails the `texkill` `< 0` test).
- **`rsq` (30).** SwARc: `inversesqrt(abs(x))` = Inf at 0, and NaN once multiplied by 0. The vertex-shader reference
  code says `if (f == 0) f = FLT_MAX`; the pixel-shader page has no code and says "a source of 0.0 yields infinity"
  (the vertex page says both). DXVK returns FLT_MAX in both stages, which is also what keeps `0 * rsq(0)` at 0 the
  way shaders written for D3D9 hardware expect. **DXVK follows the reference code; SwARc follows the prose.** For a
  renderer DXVK's is the one to match: a NaN reaches the frame buffer otherwise.
- **`rcp` (14).** Same pattern: SwARc `1.0 / x`. Vertex reference code: `if (f == 0.0f) f = FLT_MAX`; pixel page:
  prose only ("infinity"). DXVK: `min(1/x, FLT_MAX)`. The 12 pixel shaders show up as different finite colours: the
  Inf becomes a texture coordinate (projective divides by an interpolated `w` that the `edge` set made 0).
- **Overflow (3).** With `signed`/`edge` inputs three `exp`/`log` shaders exceed ~1e37; SwARc gives Inf/NaN, DXVK's
  emulation clamps `exp` and the `nrm` products to +-FLT_MAX. D3D9 does not define this; not root-caused further.
- **Read from source, not observed:** SwARc's `log` is `log2(abs(x))` (-Inf at 0; the reference code says -FLT_MAX)
  and `pow` is `pow(abs(a), b)` (undefined in GLSL for a = 0, b <= 0). No shader differed because of them: the
  game's shaders clamp before `log` (`max(x, 0.0001)`) and patching either changed no result.

In play, a zero-length normal or a zero `w` is a real input (degenerate tangent frames, black normal-map texels, a
vertex on the camera plane), so the first three are bugs to fix before use, not test artefacts.

### Real arithmetic differences

**None found.** Outside the classes above, every shader agrees within tolerance in every input set, and 94% of sampled
values are bit-identical. That covers `mov/add/mad/mul/dp3/dp4/dp2add/min/max/slt/sge/exp/log/pow/lrp/frc/cmp/abs/
sincos`, source modifiers (negate, abs), saturate, write masks and swizzles, `texld` on 2D and cube samplers,
`texldp`, `texldl`, `texkill`, depth output, `rep`, `ifc`, and `mova` with relative constant addressing (bone
indices 0-59; out-of-range indices were not exercised on purpose).

### Sampling amplification (7 pixel shaders, 1-16 pixels each)

All seven sample a cube map with a computed direction; the differing pixels are isolated or lie along one line (a
face edge), every other pixel of the same shader is bit-identical or within 1e-6. DXVK contracts multiply-adds to
`fma`, SwARc's GLSL does not, so directions differ in the last bit; at a face seam that changes the derivative and so
the mip level. Check: with mipmapping off 5 of 7 match, with filtering off as well all 7 match. One of these is the
only difference in the `benign` set.

## Not compared (the translator is only half of a backend)

SwARc leaves these to the "graphics backend" prelude; DXVK builds them into the shader. They were neutralised on both
sides, so **nothing here says SwARc's own D3D9 device gets them right**:

- alpha test, fog, clip planes, point sprites / point size, sRGB, depth-compare ("shadow") sampling and the fetch4 /
  null-texture cases DXVK switches per draw
- `vFace` sign (43 shaders): DXVK emits `gl_FrontFacing ? -1 : 1` to go with its own rasteriser setup; the test
  prelude copies that, so only the arithmetic after it was compared
- `r_position` (y flip, half-pixel offset, depth range), vertex declaration decoding (`r_fetch`: D3DCOLOR order,
  missing-component defaults, instancing), per-sampler LOD bias, texture formats
- one GPU and one MSL compiler; both sides share SPIRV-Cross and Metal, so a bug in those would not show
- compile time and pipeline-state behaviour of a Metal backend

## Recommendation

SwARc's bytecode translator is **trustworthy enough to base a native-Metal backend on, for this game**, on these
conditions:

1. Fix the zero cases to the reference results before anything else (about ten lines in `bytecode.cpp`): `nrm` and
   `rsq` and `rcp` -> FLT_MAX at 0, and for completeness `log` -> -FLT_MAX at 0. The experiment patches in
   `build_jobs.py` (`FIX_HELPERS`) are the reference; with them 106 of the 116 differing shaders match; the rest
   are the seam and overflow cases.
2. Keep this test as the regression gate: rerun it after any translator change and when new shaders are recorded.
3. Treat the backend half as unproven. The risk for a Metal backend is no longer shader arithmetic but everything in
   "Not compared", plus the instructions this game never uses if the backend is meant for other games.

What this does not decide: whether a native-Metal backend is worth building. It only removes "the translator
miscomputes" as a reason not to.

## Reproduce

`pc/tools/shader_diff/README.md`. Wall time: game run for the dump 280 s, job build 50 s, Metal run about 8 min in
one process (2 min in six), compare 3 s. Outputs in `work/shader-diff/`: `compare.txt`, `results.jsonl`,
`attribution.json`, `samples/<class>/<shader>/` (bytecode, both translations as GLSL, per-set result) for the
smallest failing shaders of each class.

## After the zero-operand fix (2026-11-15)

`pc/tools/shader_diff/swarc-zero-operands.patch` (rcp, rsq, log and nrm of 0 give FLT_MAX / -FLT_MAX, plus a `vs20_zero` case in SwARc's `hlsl_bytecode_test`; applies to SwARc 6b25b3c) and a full rerun: pixel 3,376 of 3,386 match in all four sets (was 3,280), vertex 148 of 148 (was 138). The remaining ten are the cube-seam (7) and overflow (3) classes. SwARc's other tests were not run through CMake (not installed); the bytecode test was built by hand with clang++ and passes. Native-Metal compile cost of the same shaders: plan, "Phase 5, session 9".
