#!/bin/bash
# Convert a PNG into the "SPL1" splash format that winvm-viewer reads.
#
#   SPL1 | u32 width | u32 height | width*height RGBA rows (top to bottom)
#
# Needs ImageMagick (`convert`) and python3.
set -e
src=${1:?usage: make-splash.sh splash.png [out.rgba]}
out=${2:-${src%.png}.rgba}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
w=$(identify -format '%w' "$src")
h=$(identify -format '%h' "$src")
convert "$src" -depth 8 "rgba:$tmp/raw"
python3 - "$tmp/raw" "$out" "$w" "$h" <<'PY'
import struct, sys
raw, out, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(raw, 'rb').read()
assert len(data) == w * h * 4, (len(data), w * h * 4)
with open(out, 'wb') as f:
    f.write(b'SPL1' + struct.pack('<II', w, h) + data)
print(f'wrote {out} {w}x{h}')
PY
