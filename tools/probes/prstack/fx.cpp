#include "core.h"
#include <opxy/q31.hpp>
// A CPU probe: 4,096 of one operation a block, the input passed through quietly. Not for use.
namespace prstack {
using opxy::i32;
using opxy::u32;
volatile i32 sink;
void initialize(State& state) { state.position = 0; }
void process(State& state, opxy::Buffer buffer, opxy::Channel left, opxy::Channel right, opxy::Parameters parameters) {
    (void)state; (void)parameters;
    if (buffer.words && buffer.count >= 16384) {
    volatile i32 local[64];
    for (u32 i = 0; i < 64; ++i) local[i] = 0;
    i32 acc = 0;
    for (u32 n = 0; n < 64; ++n)
        for (u32 i = 0; i < 64; ++i) { acc += local[i]; local[i] = acc; }  // a load and a store each
    sink = acc;
    }
    for (u32 i = 0; i < opxy::block_frames; ++i) { left[i] >>= 4; right[i] >>= 4; }  // some output: the benchmark counts only active runs
}
}
