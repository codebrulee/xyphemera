// Checks fragmentedearth/native/grain.s against grain_reference on the simulator (both with the counter's
// stand-in multiply). Returns the number of jobs that differ.
#include "native/grain.hpp"
extern "C" void fragmentedearth_native_grain(const opxy::i16*, opxy::i32*, fragmentedearth::GrainJob*);
static opxy::i16 rec[4096];
static opxy::i32 l1[64], r1[64], l2[64], r2[64], s1[64], s2[64];
static opxy::u32 seed = 12345;
static opxy::u32 rnd() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
extern "C" int main() {
    int bad = 0;
    for (int t = 0; t < 2000; ++t) {
        for (int i = 0; i < 4096; ++i) rec[i] = static_cast<opxy::i16>(rnd());
        fragmentedearth::GrainJob a, b;
        a.count = 1 + rnd() % 64;
        a.rate = static_cast<opxy::i32>(rnd() % 65536) - 32768;
        if (a.rate == 0) a.rate = 1;
        a.position = (1000u + rnd() % 2000u) << 14 | (rnd() & 16383);
        a.left = static_cast<opxy::i32>(rnd()) >> 1; a.step_left = static_cast<opxy::i32>(rnd()) >> 12;
        a.right = static_cast<opxy::i32>(rnd()) >> 1; a.step_right = static_cast<opxy::i32>(rnd()) >> 12;
        for (int i = 0; i < 64; ++i) { l1[i] = l2[i] = static_cast<opxy::i32>(rnd()) >> 6; r1[i] = r2[i] = static_cast<opxy::i32>(rnd()) >> 6; s1[i] = s2[i] = 7; }
        b = a;
        a.out_right = r1; a.scratch = s1;
        b.out_right = r2; b.scratch = s2;
        fragmentedearth::grain_reference(rec, l1, &a);
        fragmentedearth_native_grain(rec, l2, &b);
        bool same = a.position == b.position;
        for (int i = 0; i < 64; ++i) same = same && l1[i] == l2[i] && r1[i] == r2[i] && s1[i] == s2[i];
        if (!same) ++bad;
    }
    return bad > 255 ? 255 : bad;
}
