// Runs an effect's process() for BLOCKS blocks of noise at its defaults, for the
// GNU Blackfin simulator to count instructions (count.sh). NS is the effect's
// namespace; its folder is on the include path, so "fx.cpp" is its source.
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
        NS::process(st, opxy::Buffer{words, NS::kBufferWords}, l, r, p);
    }
    return 0;
}
