#pragma once
#include <opxy/effect.hpp>

#include "tables.h"

namespace earthshine {
// The buffer holds everything else: the tank's state at its start (Tank, in
// fx.cpp), then its delay lines and the shifters' search scratch. Its size,
// here and in definition.json, comes from tools/make_tables.py.
const opxy::u32 kBufferWords = tables::kWords;

// At most effect_buffered_state_bytes less the shift row's 4 bytes.
struct State {
    opxy::u32 cleared;  // buffer words zeroed since initialize
    opxy::u32 ready;    // the tank has been set up in the buffer
};

void initialize(State& state);
void process(State& state, opxy::Buffer buffer, opxy::Channel left, opxy::Channel right,
             opxy::Parameters parameters);
}
