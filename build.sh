#!/bin/sh
# Builds bin/mp4player.sis (ARMI, for the Nokia 9300 / 9500, Series 80 2.0).
#   default: gcc -O2 (set in group/mp4player.mmp); OPT=O ./build.sh builds bin/mp4player_O.sis with -O
# Needs the EKA1 toolchain and the Series 80 SDK, set up by the main repo's
#   symbian-build/toolchain/setup_eka1_toolchain.sh <dir> ~/sym
#   symbian-build/toolchain/setup_s80_sdk.sh S80_DP_2_0_SDK.zip ~/sym
set -e
SYM=${SYM:-$HOME/sym}
HERE=$(cd "$(dirname "$0")" && pwd)
export EPOCROOT=$SYM/s80_20/ PATH=$SYM/wrap:$PATH
GROUP=group
SIS=mp4player.sis
if [ -n "$OPT" ]; then
  GROUP=group_$OPT
  SIS=mp4player_$OPT.sis
  rm -rf "$HERE/$GROUP"; mkdir "$HERE/$GROUP"
  cp "$HERE"/group/* "$HERE/$GROUP/"
  printf '\nOPTION GCC -%s\n' "$OPT" >> "$HERE/$GROUP/mp4player.mmp"
fi
cd "$HERE/$GROUP"
bldmake bldfiles
abld reallyclean armi urel >/dev/null 2>&1 || true
abld build armi urel 2>&1 | tee /tmp/mp4player_build.log
if grep -q -E "Error [0-9]|error:" /tmp/mp4player_build.log; then echo "BUILD FAILED"; exit 1; fi
E=$SYM/s80_20/epoc32
mkdir -p "$HERE/bin"
sed "s|EPOC32|$E|g; s|HERE|$HERE|g" "$HERE/sis/mp4player.pkg.in" > "$HERE/bin/mp4player.pkg"
cd "$HERE/bin" && makesis mp4player.pkg "$SIS" && rm mp4player.pkg
[ -n "$OPT" ] && rm -rf "$HERE/$GROUP"
ls -l "$HERE/bin/$SIS"
