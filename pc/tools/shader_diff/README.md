# shader_diff: numeric differential test of two D3D9 shader translators (macOS, Metal)

Runs every shader of a DXVK warm file (`pipelines.warm.shaders`) through two translations of the same bytecode and
compares what they compute on the GPU:

- **swarc**: SwARc's `hlsl::TranslateBytecode` (GLSL) -> `glslangValidator` -> SPIR-V
- **dxvk**: the SPIR-V DXVK 3.1.1 itself produced (dxbc-spirv's SM3 front end plus DXVK's D3D9 lowering)

Both go through `spirv-cross --msl` and are compiled by Metal with fast math off. Results and findings:
`docs/todo/SHADER-DIFF.md`. Nothing here contains game data; everything the tools write does, so point them at `work/`.

| File | What it does |
|---|---|
| `run.sh` | the whole thing: build tools, translate, build jobs, run, compare |
| `translate.cpp` | SwARc over every record; writes complete GLSL (with a neutral prelude) and the interface it reports |
| `spvpatch.py` | DXVK's unsized sampler heap -> a 1-element array, so plain MSL can be emitted |
| `sm3.py` | bytecode reader: warm-file records, opcodes/features, `def`s, `dcl`s; run it for opcode statistics |
| `build_jobs.py` | MSL for both sides, the driver code, and identical inputs matched by register number and semantic |
| `runner.swift` | Metal: pixel shaders drawn into RGBA32F + Depth32F, vertex shaders called from a compute kernel |
| `compare.py` | tolerance compare, classes, per-feature table, smallest failing shaders |

## Getting DXVK's SPIR-V

One bounded game run with the warm file (the launch-time rebuild creates every recorded shader), on scratch paths:

```sh
S=$PWD/work/shader-diff; mkdir -p $S/dxvk-dump $S/warm
cp ~/.cache/dxvk/pipelines.warm ~/.cache/dxvk/pipelines.warm.shaders $S/warm/
DXVK_SHADER_DUMP_PATH=$S/dxvk-dump DXVK_PIPELINE_WARM_FILE=$S/warm/pipelines.warm \
  ME_PC_DXVK=$PWD/work/dxvk/bin/libdxvk_d3d9.0.dylib sh work/run-bounded.sh <me_pc> $S/run 280
pc/tools/shader_diff/run.sh $S/warm/pipelines.warm.shaders $S/dxvk-dump work/swarc $S
```

Dumping turns DXVK's shader cache off, and the scratch warm file keeps `~/.cache/dxvk` untouched.

## How the two sides get the same inputs

- Constant registers: one random value per register number; a register the shader `def`s holds the shader's value.
  DXVK's buffers are packed and name their members `c<register>_<name>`; SwARc's is `c[n]`.
- Varyings and vertex inputs: one value per D3D9 semantic. SwARc uses a fixed location per semantic
  (`VaryingLocation`), DXVK names its variables `v<reg>_<semantic>` / `o<reg>_<semantic>`.
- Textures: per sampler register, shared 2D/cube/3D RGBA16F textures whose mip levels hold unrelated random values
  (a wrong LOD shows), one sampler state (linear, mip-linear, repeat).
- DXVK's run-time state is set to "nothing": alpha test always passes, no fog, no clip planes, no point sprites,
  no depth-compare sampling, `cFSize` 256. SwARc's prelude is equally neutral (`translate.cpp`).
- Four input sets: `benign` (positive, plausible), `signed`, `edge` (zeros and ones mixed in), `zero` (everything 0).

`build_jobs.py --only <names> --swarc-fix rcp,rsq,nrm,log,pow` rebuilds chosen shaders with SwARc's output patched to
the D3D9 reference results for a zero operand; a difference that disappears with one patch is attributed to it.
`SD_NOMIP=1` / `SD_NEAREST=1` (runner) take texture LOD and filtering out of the picture.
