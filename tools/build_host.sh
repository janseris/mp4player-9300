#!/bin/sh
# Builds the PC test decoder test/m4vtest (all codecs) with the host compiler.
# usage: tools/build_host.sh [output] [extra gcc flags]
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/test/m4vtest}
shift 2>/dev/null
set -e
T=$(mktemp -d)
for f in "$HERE"/core/h264/*.c; do gcc -O2 -fno-strict-aliasing "$@" -c -I"$HERE/core/h264" -o "$T/$(basename "$f" .c).o" "$f"; done
g++ -O2 -fno-strict-aliasing "$@" -I"$HERE/core" -o "$OUT" "$HERE/test/m4vtest.cpp" "$HERE"/core/*.cpp "$T"/*.o
rm -rf "$T"
