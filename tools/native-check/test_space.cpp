// Checks earthgrain/native/space.s against space_reference on the simulator (both with the counter's
// stand-in multiply): random jobs, 200 blocks each, the position stepping on as fx.cpp's does.
// Returns the number of jobs that differ.
#ifndef TRIALS
#define TRIALS 300
#endif
#include "native/space.hpp"
using namespace earthgrain;
static opxy::i32 w1[kSpaceWords], w2[kSpaceWords], s1[160], s2[160], l1[64], l2[64], r1[64], r2[64];
static opxy::u32 seed = 777;
static opxy::u32 rnd() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static opxy::i32 small() { return static_cast<opxy::i32>(rnd()) >> 4; }
extern "C" int main() {
    int bad = 0;
    for (int t = 0; t < TRIALS; ++t) {
        for (opxy::u32 i = 0; i < kSpaceWords; ++i) w1[i] = w2[i] = static_cast<opxy::i32>(rnd()) >> 3;
        SpaceJob a, b;
        a.at = rnd() & kSpaceMask;
        a.dry = static_cast<opxy::i32>(rnd() >> 1); a.wet = static_cast<opxy::i32>(rnd() >> 1);
        for (int k = 0; k < 4; ++k) { a.gain[k] = static_cast<opxy::i32>(rnd() >> 1); a.damp[k] = small(); }
        a.last[0] = small(); a.last[1] = small();
        b = a;
        bool same = true;
        for (int blk = 0; blk < 200 && same; ++blk) {
            for (int i = 0; i < 64; ++i) { l1[i] = l2[i] = small(); r1[i] = r2[i] = small(); }
            for (int i = 0; i < 160; ++i) s1[i] = s2[i] = small();
            a.left = l1; a.right = r1; b.left = l2; b.right = r2;
            b.at = a.at;
            space_reference(w1, s1, &a);
            earthgrain_native_space(w2, s2, &b);
            for (int i = 0; i < 64; ++i) same = same && l1[i] == l2[i] && r1[i] == r2[i];
            for (int k = 0; k < 4; ++k) same = same && a.damp[k] == b.damp[k];
            same = same && a.last[0] == b.last[0] && a.last[1] == b.last[1];
            for (opxy::u32 i = 0; i < kSpaceWords; ++i) same = same && w1[i] == w2[i];
            a.at = (a.at - kSpaceHalf) & kSpaceMask;
        }
        if (!same) ++bad;
    }
    return bad > 255 ? 255 : bad;
}
