#!/bin/bash
# Checks a machine's native routines against their C references on the GNU Blackfin
# simulator, both built with the instruction counter's stand-in multiply (the simulator
# lacks the 32 x 32 (T) multiply, so each "Rx = Ry * Rz (T)" becomes the integer one in the
# routine too; everything else runs as on the device). The test returns how many cases differ.
#
#   tools/native-check/check.sh <machine folder> <test.cpp> <routine.s ...>
#
# e.g. tools/native-check/check.sh earthgrain tools/native-check/test_space.cpp earthgrain/native/space.s
# Needs OPEN_XY (the SDK checkout, default ../open-xy beside this repository) and BFIN_RUN
# (bfin-elf-run from GNU binutils-gdb's sim, default: on PATH).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OX=${OPEN_XY:-$HERE/../../../open-xy}; T=$OX/local/toolchain/bin; SDK=$OX/sdk
TOOL=$HERE/../blackfin-count
FE=$(cd "$1" && pwd); shift
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FLAGS="-mcpu=bf707 -O2 -std=gnu++11 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common -fno-tree-loop-distribute-patterns -fomit-frame-pointer -nostdinc++ -I$TOOL/include -I$SDK/include -I$SDK/platform/blackfin/freestanding -I$FE"
"$T/bfin-elf-as" -mcpu=bf707 -o "$W/start.o" "$TOOL/start.s"
"$T/bfin-elf-as" -mcpu=bf707 -o "$W/divide.o" "$SDK/platform/blackfin/divide.s"
"$T/bfin-elf-g++" $FLAGS -c "$SDK/platform/blackfin/memory.cpp" -o "$W/memory.o"
TEST=$1; shift
objs=()
for s in "$@"; do
  b=$(basename $s .s)
  # the simulator lacks the 32 x 32 (T) multiply: the integer one stands in, as in the counter's q31.hpp
  sed -E -e 's/^(\s*)(R[0-7]) = (R[0-7]) \* (R[0-7]) \(T\) \|\| ([^|]*) \|\| ([^|;]*);/\1\2 = \3; \2 *= \4; \5; \6;/' -e 's/^(\s*)(R[0-7]) = (R[0-7]) \* (R[0-7]) \(T\);/\1\2 = \3; \2 *= \4;/' "$s" > "$W/$b.s"
  grep -q "(T)" "$W/$b.s" && { echo "unconverted (T) multiply in $s"; exit 3; }
  "$T/bfin-elf-as" -mcpu=bf707 -o "$W/$b.o" "$W/$b.s"
  objs+=("$W/$b.o")
done
"$T/bfin-elf-g++" $FLAGS ${DEFS:-} -c "$TEST" -o "$W/test.o"
"$T/bfin-elf-ld" -e __start -o "$W/test.elf" "$W/start.o" "$W/test.o" "${objs[@]}" "$W/divide.o" "$W/memory.o"
set +e
${BFIN_RUN:-bfin-elf-run} --environment user ${VERBOSE:+-v} "$W/test.elf" > "$W/out.txt" 2>&1
code=$?
[ -n "${VERBOSE:-}" ] && grep "Total instructions" "$W/out.txt"
[ $code -eq 0 ] && echo "PASS: no case differs" || echo "FAIL: $code cases differ (255: at least)"
exit $code
