#pragma once
#include <opxy/effect.hpp>
namespace prbuf {
struct State { opxy::u32 position; };
void initialize(State& state);
void process(State& state, opxy::Buffer buffer, opxy::Channel left, opxy::Channel right, opxy::Parameters parameters);
}
