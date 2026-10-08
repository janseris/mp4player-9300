#!/bin/bash
# Host regression test: decodes test clips with the core and compares every frame with ffmpeg
# (bit-exact expected). Needs ffmpeg with the mpeg4/h263/libxvid encoders, python3, g++.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=${T:-/tmp/mp4player-tests}
mkdir -p "$T"
"$HERE/tools/build_host.sh" "$T/m4vtest"
cd "$T"
S="-f lavfi -i testsrc2=size=352x288:rate=15:duration=4"
gen() { [ -f "$1" ] || ffmpeg -hide_banner -loglevel error -y $S "${@:2}" -an "$1"; }
gen qcif.mp4 -vf scale=176:144 -c:v mpeg4 -q:v 5 -g 30
gen qvga_4mv.mp4 -vf scale=320:240 -c:v mpeg4 -b:v 400k -flags +mv4 -mbd rd -trellis 1
gen mpegquant.mp4 -vf scale=320:240 -c:v mpeg4 -q:v 4 -mpeg_quant 1
gen resync.mp4 -vf scale=320:240 -c:v mpeg4 -q:v 6 -ps 400
gen h263.3gp -vf scale=176:144 -c:v h263 -q:v 6
gen odd.mp4 -vf scale=200:150 -c:v mpeg4 -q:v 5
gen aq.mp4 -vf scale=320:240 -c:v mpeg4 -q:v 5 -lumi_mask 0.3 -dark_mask 0.3 -p_mask 0.3
H="-pix_fmt yuv420p -c:v libx264 -profile:v baseline -bf 0"
gen h264_426x240.mp4 -vf scale=426:240 $H -b:v 300k -g 30
gen h264_320x180_slices.mp4 -vf scale=320:180 $H -crf 26 -slices 4 -refs 4
gen h264_noise.mp4 -vf scale=426:240,noise=alls=40:allf=t $H -b:v 400k
gen h264_nodeblock.mp4 -vf scale=426:240 $H -x264-params no-deblock=1:constrained-intra=1 -b:v 300k
for f in *.mp4 *.3gp; do
  ./m4vtest "$f" mine.yuv 2> log.txt
  ffmpeg -hide_banner -loglevel error -y -idct simple -flags +bitexact -i "$f" -f rawvideo -pix_fmt yuv420p ref.yuv
  echo -n "$f: "; python3 "$HERE/tools/cmp.py" mine.yuv ref.yuv $(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$f" | tr , " ")
done
