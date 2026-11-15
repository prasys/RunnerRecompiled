#!/bin/sh
# Shader differential test, end to end. Everything it writes is derived from game shaders: keep <work> under work/.
# usage: run.sh <pipelines.warm.shaders> <dxvk dump dir> <swarc checkout> <work dir>
# The DXVK dump comes from one game run with DXVK_SHADER_DUMP_PATH=<dir> (see README.md).
set -e
SH=$1; DUMP=$2; SWARC=$3; W=$4
T=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W/bin" "$W/swarc"
clang++ -std=c++20 -O1 -I"$SWARC/src" "$T/translate.cpp" "$SWARC"/src/runtime/hlsl/*.cpp -o "$W/bin/translate"
swiftc -O "$T/runner.swift" -o "$W/bin/runner"
"$W/bin/translate" "$SH" "$W/swarc"
python3 -I "$T/build_jobs.py" --shaders "$SH" --dump "$DUMP" --work "$W"
"$W/bin/runner" "$W/jobs.jsonl" "$W/textures.bin"
python3 -I "$T/compare.py" --work "$W" --json "$W/results.jsonl" | tee "$W/compare.txt"
