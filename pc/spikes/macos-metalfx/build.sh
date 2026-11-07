#!/bin/sh
set -eu
cd "$(dirname "$0")"
swiftc -O -framework AppKit -framework Metal -framework MetalFX -framework QuartzCore main.swift -o ../../../work/metalfx-spike
