#!/bin/sh
set -eu
cd "$(dirname "$0")"
swiftc -O -framework AppKit -framework Metal -framework MetalFX -framework QuartzCore main.swift -o ../../../work/metalfx-spike
exec ../../../work/metalfx-spike "$PWD/../../../work/frames/frame_900.ppm" "$PWD/../../../work/metalfx-out"
