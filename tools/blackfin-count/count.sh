#!/bin/bash
# Steady-state Blackfin instructions per block for an effect, from the GNU
# Blackfin simulator: the difference between runs of 20 and 60 blocks, over 40.
#
#   tools/blackfin-count/count.sh <effect folder> <namespace> [parameters]
#
# parameters: the twelve words process() gets (default: 0.5 for the eight
# controls, the Shift row at its first choices). Needs:
#   OPEN_XY     the SDK checkout (default ../open-xy beside this repository)
#   BFIN_RUN    bfin-elf-run from GNU binutils-gdb's sim (default: on PATH)
# The simulator can't run the OP-XY's 32 x 32 fractional multiply, so
# include/opxy/q31.hpp stands in a one-instruction integer multiply: the audio
# is wrong but the instruction count is right. It counts instructions, not
# cycles: on the device a cycle count is about 1.7 times this (measured), and
# cache misses depend on memory use. Only a profiling build measures CPU.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
EFFECT=$(cd "$1" && pwd); NS=$2
PARAMETERS=${3:-"1073741823,1073741823,1073741823,1073741823,1073741823,1073741823,1073741823,1073741823,0,0,0,0"}
OPEN_XY=${OPEN_XY:-$HERE/../../../open-xy}
BFIN_RUN=${BFIN_RUN:-bfin-elf-run}
T=$OPEN_XY/local/toolchain/bin; SDK=$OPEN_XY/sdk
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$EFFECT"/*.cpp "$EFFECT"/*.h "$W"/
# Clear the buffer at once, so start-up doesn't count.
sed -i 's/const u32 kClearPerBlock = [0-9u <]*;/const u32 kClearPerBlock = 1u << 20;/' "$W/fx.cpp"
FLAGS="-mcpu=bf707 -O2 -std=gnu++11 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-common -fno-tree-loop-distribute-patterns -fomit-frame-pointer -nostdinc++ -I$HERE/include -I$SDK/include -I$SDK/platform/blackfin/freestanding -I$W"
"$T/bfin-elf-as" -mcpu=bf707 -o "$W/start.o" "$HERE/start.s"
"$T/bfin-elf-as" -mcpu=bf707 -o "$W/divide.o" "$SDK/platform/blackfin/divide.s"
"$T/bfin-elf-g++" $FLAGS -c "$SDK/platform/blackfin/memory.cpp" -o "$W/memory.o"
n=()
for b in 20 60; do
    "$T/bfin-elf-g++" $FLAGS -DNS=$NS -DBLOCKS=$b "-DPARAMETERS=$PARAMETERS" -c "$HERE/bench.cpp" -o "$W/bench.o"
    "$T/bfin-elf-ld" -e __start -o "$W/bench$b.elf" "$W/start.o" "$W/bench.o" "$W/divide.o" "$W/memory.o"
    n+=($("$BFIN_RUN" --environment user -v "$W/bench$b.elf" 2>&1 | grep "Total instructions" | tr -d ', ' | sed 's/.*:\([0-9]*\).*/\1/'))
done
echo $(( (n[1] - n[0]) / 40 ))
