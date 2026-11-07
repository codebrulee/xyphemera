# Shared tools

## blackfin-count

`count.sh <effect folder> <namespace> [parameters]` prints an effect's
steady-state Blackfin instructions per block, run on the GNU Blackfin simulator
(`bfin-elf-run`). Set `OPEN_XY` to the SDK checkout and `BFIN_RUN` to the
simulator. The simulator cannot run the OP-XY's 32 x 32 fractional multiply, so
`include/opxy/q31.hpp` stands in a one-instruction integer multiply: the audio
is wrong, the count right. Device cycles have measured about 1.7 per counted
instruction; only a profiling build measures CPU.

The simulator is GNU binutils-gdb's `sim/bfin`, built with
`--target=bfin-elf --enable-sim --disable-gdb` (older releases need
`-DSIGSTKSZ=8192` and readline turned off).

## render

`abrender.cpp` renders a test phrase (pad chords, a plucked melody, a tail)
through an effect's fixed-point code to `<out>_wet.wav` and `<out>_mix.wav`.
Build it with the effect's folder on the include path, its namespace as `NS`
and its twelve parameter words as `PARAMETERS` (see the file). Give every file of
an A/B the same gain.

## probes

Four effects that each do 4,096 of one operation a block, for a profiling
build to time on the device: stack load + store (`prstack`), buffer load +
store in order (`prbuf`) and scattered (`prscatter`), and multiplies
(`prmul`). Not for release. Add them to a profiling collection; they need a
buffer the benchmark can allocate (64 KB here; 256 KB failed) and pass audio
through, or the profiler counts them inactive.
