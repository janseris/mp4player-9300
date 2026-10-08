#!/bin/sh
# Compiles the decoder core with the phone's compiler and flags (GCC 2.9, ARMI, -O2) into a flat
# binary at 0x10000 for the Unicorn emulator (run.py). Needs ~/sym from the main build setup.
set -e
SYM=${SYM:-$HOME/sym}
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../../core
OUT=${OUT:-/tmp/armemu}
mkdir -p "$OUT"
rm -f "$OUT"/*.o
export PATH=$SYM/gcc539/bin:$PATH
CFLAGS="-march=armv4t -mthumb-interwork -pipe -c -nostdinc -fomit-frame-pointer -O2 ${EXTRA_CFLAGS}"
for f in "$CORE/m4vdec.cpp" "$CORE/idct.cpp" "$CORE/yuv2rgb.cpp" "$CORE/mp4demux.cpp" "$CORE/viddec.cpp" "$HERE/emu_rt.cpp"; do
  arm-epoc-pe-gcc $CFLAGS -I "$CORE" -o "$OUT/$(basename "$f" .cpp).o" "$f"
done
for f in "$CORE"/h264/*.c "$HERE/ktest.c"; do
  arm-epoc-pe-gcc $CFLAGS -I "$CORE/h264" -o "$OUT/$(basename "$f" .c).o" "$f"
done
LIBGCC=$(arm-epoc-pe-gcc -print-libgcc-file-name)
arm-epoc-pe-ld -o "$OUT/core.exe" -Ttext 0x10000 --image-base 0 -e emu_heap_init "$OUT"/*.o "$LIBGCC"
arm-epoc-pe-nm "$OUT/core.exe" > "$OUT/core.sym"
arm-epoc-pe-objdump -h "$OUT/core.exe" > "$OUT/core.sections"
echo "built $OUT/core.exe"
