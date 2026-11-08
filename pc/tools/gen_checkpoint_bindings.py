#!/usr/bin/env python3
"""Unattended checkpoint tour for the pipeline warm-up (issue #11/#12).

The engine takes `start <Map>?LoadCheckpoint=<name>` as a console command, and TdInput.ini can bind a key to a chain of
them (the technique from https://gist.github.com/LucasOe/1f5f89b4ea79361dcb4b0ae654cafff1). This script writes such a chain
into a COPY of a user profile's TdInput.ini: one key (default H) loads the next checkpoint on every press. Pair it with
ME_PC_AUTO_INPUT, which presses the key at given D3D9 frame numbers (frames do not advance during a compile stall, so a
cold compile cannot make the presses collide), and the DXVK pipeline warm file fills with every map.

usage: gen_checkpoint_bindings.py <TdInput.ini> <checkpoints.txt> [--key H] [--maps Edge_p,Escape_p] [--limit N]
                                  [--first-frame 2600] [--spacing 1500]
  checkpoints.txt: one `start Map?LoadCheckpoint=Name` per line (extract it from the gist's cpN lines; it is not game data
                   but it is not kept in the repo either: see work/record-all/).
Prints the ME_PC_AUTO_INPUT value to use. Edits the ini in place: pass a copy of the profile, never the real one.
"""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument("ini"); ap.add_argument("list")
ap.add_argument("--key", default="H"); ap.add_argument("--maps", default="")
ap.add_argument("--limit", type=int, default=0)
ap.add_argument("--first-frame", type=int, default=2600); ap.add_argument("--spacing", type=int, default=1500)
a = ap.parse_args()

cmds = [l.strip() for l in open(a.list, encoding="utf-8") if l.strip().startswith("start ")]
if a.maps:
    keep = set(a.maps.split(","))
    cmds = [c for c in cmds if re.match(r"start (\w+)\?", c).group(1) in keep]
if a.limit:
    cmds = cmds[:a.limit]
if not cmds:
    sys.exit("no checkpoints selected")

tail = 'Control=False,Shift=False,Alt=False)\r\n'
lines = [f'Bindings=(Name="{a.key}",Command="rec1",{tail}'.replace(",Control", ",Control", 1)]
for i, c in enumerate(cmds, 1):
    nxt = f" | setbind {a.key} rec{i + 1}" if i < len(cmds) else ""
    lines.append(f'Bindings=(Name="rec{i}",Command="{c}{nxt}",{tail}')

s = open(a.ini, newline="").read()
marker = "bEnableMouseSmoothing=true\r\n"
if marker not in s:
    sys.exit("marker bEnableMouseSmoothing=true not found (is this a TdInput.ini with CRLF?)")
s = re.sub(r'Bindings=\(Name="(rec\d+|%s)",Command="(rec\d+|start [^"]*)",Control=False,Shift=False,Alt=False\)\r\n' % re.escape(a.key), "", s)
open(a.ini, "w", newline="").write(s.replace(marker, marker + "".join(lines), 1))

acts = []
for i in range(len(cmds)):
    f = a.first_frame + i * a.spacing
    acts += [f"{f}:down={a.key}", f"{f + 10}:up={a.key}"]
print(";".join(acts))
sys.stderr.write(f"{len(cmds)} checkpoints, last press at frame {a.first_frame + (len(cmds)-1) * a.spacing}\n")
