#!/bin/bash
# Decodes every H.264 clip in DIR (default /tmp/h264set) on the PC and in the ARM emulator (code built
# by the phone's compiler, build.sh) and compares the picture checks. Needs DIR/m4vtest (build_host.sh).
DIR=${1:-/tmp/h264set}
HERE=$(cd "$(dirname "$0")" && pwd)
N=${N:-150}
one() {
  f=$1
  host=$(CHECKN=$N "$DIR/m4vtest" "$f" /dev/null 2>&1 | sed -n 's/^check (first [0-9]* samples): //p')
  emu=$(python3 "$HERE/run.py" h264 "$f" $N | sed -n 's/.*check \([0-9A-F]*\).*avg \([0-9]*\).*/\1 \2/p')
  set -- $emu
  if [ "$host" = "$1" ]; then r=ok; else r=MISMATCH; fi
  printf "%-30s %-8s host %s emu %s  %s instr/frame\n" "$(basename "$f")" $r "$host" "$1" "$2"
}
export -f one; export DIR HERE N
ls "$DIR"/*.mp4 | xargs -P ${JOBS:-8} -I{} bash -c 'one {}' | sort
