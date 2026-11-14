// Runs an effect's process() for BLOCKS blocks of noise at its defaults, for the
// GNU Blackfin simulator to count instructions (count.sh). NS is the effect's
// namespace; its folder is on the include path, so "fx.cpp" is its source. Define
// BPM_X10 for an effect that declares "transport" (count.sh passes it through).
#include "fx.cpp"
static opxy::i32 words[NS::kBufferWords];
extern "C" int main() {
    NS::State st;
    NS::initialize(st);
    const opxy::i32 p[12] = {PARAMETERS};
    opxy::i32 l[64], r[64];
    opxy::u32 seed = 1;
    for (int b = 0; b < BLOCKS; ++b) {
        for (int i = 0; i < 64; ++i) l[i] = r[i] = static_cast<opxy::i32>(seed = seed * 1664525u + 1013904223u) >> 4;
#ifdef BPM_X10
        NS::process(st, opxy::Buffer{words, NS::kBufferWords}, opxy::Transport{BPM_X10, static_cast<opxy::u32>(b), true}, l, r, p);
#else
        NS::process(st, opxy::Buffer{words, NS::kBufferWords}, l, r, p);
#endif
    }
    return 0;
}
