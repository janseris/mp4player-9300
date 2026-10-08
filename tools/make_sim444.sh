#!/bin/sh
# Test clips like the 444x250 internet video (analysed on the phone with 1.25): x264 "ultrafast"
# H.264 Baseline (CAVLC, 1 reference frame, deblocking filter off in the stream, 1 slice), level 3.0,
# 25 fps, ~150 kbit/s, a key frame every 12 frames, mostly still picture (average QP ~30),
# AAC-LC mono 22050 Hz ~24 kbit/s. Two files: the sample entry saying 2 channels like the original
# (MP4Box writes 2 for any AAC) and a correct one saying 1.
# usage: tools/make_sim444.sh [outdir]
set -e
OUT=${1:-samples}
T=$(mktemp -d)
ffmpeg -hide_banner -v error -y -f lavfi -i "mandelbrot=s=444x250:r=1:maxiter=300:start_scale=1.5" -frames:v 1 "$T/still.png"
ffmpeg -hide_banner -v error -y -loop 1 -framerate 25 -i "$T/still.png" -f lavfi -i testsrc2=s=96x72:r=25 \
  -f lavfi -i "sine=frequency=440:sample_rate=22050,volume=0.3" \
  -filter_complex "[0]gblur=sigma=3[b];[1]gblur=sigma=1.5[o];[b][o]overlay=x='mod(t*20,348)':y=90,format=yuv420p[v]" \
  -map "[v]" -map 2 -t 60 -c:v libx264 -preset ultrafast -profile:v baseline -level 3.0 -g 12 -b:v 125k \
  -c:a aac -ac 1 -ar 22050 -b:a 24k -movflags +faststart "$OUT/sim444_mono_header.mp4"
python3 - "$OUT/sim444_mono_header.mp4" "$OUT/sim444_like_original.mp4" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
f = d.find(b'mp4a')
d[f + 20:f + 22] = b'\x00\x02'      # sample entry channelcount: 2
open(sys.argv[2], 'wb').write(d)
PY
rm -rf "$T"
ls -l "$OUT"/sim444_*.mp4
