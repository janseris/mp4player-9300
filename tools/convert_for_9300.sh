#!/bin/sh
# Converts any video into a file MP4 Player 9300 plays well on the Nokia 9300 / 9500.
# Settings follow what worked for SmartMovie on the Communicators: at most 320x200 (the full
# screen height; 640 wide is too much for the 150 MHz CPU), 15 fps, ~1.5 MB per minute.
#   mpeg4 (default): MPEG-4 Part 2 Simple Profile, no B-frames, ~200 kbit/s - the fastest to decode
#   h264:            H.264 Baseline, ~160 kbit/s - better picture per MB, ~1.5x the decoding work
#   AAC mono 22 kHz 32 kbit/s
# usage: convert_for_9300.sh input [fit|crop] [mpeg4|h264] [output.mp4]   (options in any order)
#   fit  (default) whole picture, e.g. 16:9 -> 320x180, 4:3 -> 266x200
#   crop fills 320x200 by cutting off the sides (16:9) or top and bottom (4:3)
[ -n "$1" ] || { echo "usage: $0 input [fit|crop] [mpeg4|h264] [output.mp4]"; exit 1; }
IN=$1; shift
MODE=fit; CODEC=mpeg4; OUT=
for a in "$@"; do
  case "$a" in
    fit|crop) MODE=$a ;;
    mpeg4|h264) CODEC=$a ;;
    *) OUT=$a ;;
  esac
done
[ -n "$OUT" ] || OUT=${IN%.*}_9300.mp4
if [ "$MODE" = crop ]; then
  VF="scale=320:200:force_original_aspect_ratio=increase,crop=320:200,fps=15"
else
  VF="scale=320:200:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=15"
fi
if [ "$CODEC" = h264 ]; then
  V="-c:v libx264 -profile:v baseline -preset slow -bf 0 -g 60 -b:v 160k -maxrate 320k -bufsize 640k -pix_fmt yuv420p"
else
  V="-c:v mpeg4 -vtag mp4v -bf 0 -g 60 -b:v 200k -maxrate 400k -bufsize 800k"
fi
exec ffmpeg -i "$IN" -map 0:v:0 -map '0:a:0?' -vf "$VF" $V \
  -c:a aac -ac 1 -ar 22050 -b:a 32k -movflags +faststart "$OUT"
