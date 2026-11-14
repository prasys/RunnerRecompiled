# Splits a game run's log into one file per map load and drops the noisy lines.
#   me_pc ... 2>&1 | awk -v D=<outdir> -f pc/tools/record_split.awk      (creates <outdir>/logs/NN-<map>.log, <outdir>/noise.txt)
# Use with ME_PC_LOG_FILES=1 (lines then carry a [HH:MM:SS] prefix, which is why the map marker is matched anywhere in the line).
# Dropped: "file not found" / .uncompressed_size lookups and hle/stub lines (counted in noise.txt). Everything else is kept.
function openchunk(name) { if (out != "") close(out); n++; out = sprintf("%s/logs/%02d-%s.log", D, n, name); cur = name }
BEGIN { system("mkdir -p " D "/logs"); openchunk("boot") }
/warm-up map: / { i = index($0, "warm-up map: "); m = substr($0, i + 13); sub(/[ \r].*$/, "", m); if (m != cur) openchunk(m) }
/CreateFile .*(not found|\.uncompressed_size)/ || /^  hle / || /^stub / || /^hle / { noise++; if (noise % 1000 == 0) print noise > (D "/noise.txt"); next }
{ print >> out; fflush(out) }
END { print noise > (D "/noise.txt") }
