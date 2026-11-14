#pragma once
#include <opxy/native.hpp>
#include <opxy/q31.hpp>
#include <opxy/types.hpp>

// Space's block (fx.cpp's space(), once it is set up): down to half rate, two diffusers each
// side, the loop's four lines and back to full rate. The device runs native/space.s.
namespace fragmentedearth {
// Its eight lines share 4096 words: one position turns through them all, each line writing at
// its own start and reading its own length further on. (In samples at half rate.)
const opxy::u32 kSpaceWords = 4096;
const opxy::u32 kSpaceMask = kSpaceWords - 1;
const opxy::u32 kSpaceLengths[8] = {71, 191, 53, 139, 523, 661, 761, 937};
// Each line's start: one after the end of the last, so none overwrites another's samples.
const opxy::u32 kSpaceStart[8] = {0, 72, 264, 318, 458, 982, 1644, 2406};
const opxy::i32 kSpaceDamp = 1395864371;  // 0.65 (Q31): how much of each line's change gets through
const opxy::i32 kSpaceMost = 1 << 28;
const opxy::u32 kSpaceHalf = 32;          // half-rate samples a block
struct SpaceJob {
    opxy::i32* left;     // the block, both read and written (64 samples)
    opxy::i32* right;
    opxy::u32 at;        // the position, 0..4095 (fx.cpp moves it on after)
    opxy::i32 dry, wet;  // Q31
    opxy::i32 gain[4];   // the loop lines' gains, Q31
    opxy::i32 damp[4];   // the lines' darkening, carried block to block
    opxy::i32 last[2];   // each side's last half-rate sample, carried block to block
};
}

extern "C" void fragmentedearth_native_space(opxy::i32* words, opxy::i32* scratch, fragmentedearth::SpaceJob* job);

namespace fragmentedearth {
inline opxy::i32 space_limit(opxy::i32 x) { return x > kSpaceMost ? kSpaceMost : x < -kSpaceMost ? -kSpaceMost : x; }
// One diffuser (an allpass of gain 0.625) for one sample, its line at `k`, position m.
inline opxy::i32 space_diffuse(opxy::i32* words, opxy::u32 m, opxy::u32 k, opxy::i32 in) {
    const opxy::i32 d = words[(m + kSpaceStart[k] + kSpaceLengths[k]) & kSpaceMask];
    const opxy::i32 v = space_limit(in - (d >> 1) - (d >> 3));
    words[(m + kSpaceStart[k]) & kSpaceMask] = v;
    return d + (v >> 1) + (v >> 3);
}
// The definition, a half-rate sample at a time. `scratch` holds the dry input at half rate
// (32 words each side) and has 96 more words for the device routine; it is left undefined.
// Every line is longer than a block, so a block's reads were all written in earlier blocks
// and the order of the work within it doesn't matter (the device routine goes stage by stage).
inline void space_reference(opxy::i32* words, opxy::i32* scratch, SpaceJob* job) {
    using opxy::q31::multiply;
    const opxy::i32* dl = scratch;
    const opxy::i32* dr = scratch + kSpaceHalf;
    opxy::i32* left = job->left;
    opxy::i32* right = job->right;
    const opxy::u32 r4 = kSpaceStart[4] + kSpaceLengths[4], r5 = kSpaceStart[5] + kSpaceLengths[5];
    const opxy::u32 r6 = kSpaceStart[6] + kSpaceLengths[6], r7 = kSpaceStart[7] + kSpaceLengths[7];
    opxy::i32 d4 = job->damp[0], d5 = job->damp[1], d6 = job->damp[2], d7 = job->damp[3];
    opxy::i32 last_left = job->last[0], last_right = job->last[1];
    opxy::u32 m = job->at;
    for (opxy::u32 i = 0; i < kSpaceHalf; ++i, m = (m - 1) & kSpaceMask) {
        // Down to half rate: each pair of samples averaged (and halved, for headroom), added to
        // the dry input, so what is played has the space too.
        opxy::i32 xl = dl[i] + (left[2 * i] >> 2) + (left[2 * i + 1] >> 2);
        opxy::i32 xr = dr[i] + (right[2 * i] >> 2) + (right[2 * i + 1] >> 2);
        xl = space_diffuse(words, m, 1, space_diffuse(words, m, 0, xl));
        xr = space_diffuse(words, m, 3, space_diffuse(words, m, 2, xr));
        const opxy::i32 a4 = words[(m + r4) & kSpaceMask], a5 = words[(m + r5) & kSpaceMask];
        const opxy::i32 a6 = words[(m + r6) & kSpaceMask], a7 = words[(m + r7) & kSpaceMask];
        // The loop: what comes round less half the loop's sum (the Householder mix), darkened,
        // times its gain, plus the input (minus it for the second pair). Lines 4 and 6 take the
        // left, 5 and 7 the right; 4 and 7 give the left, 5 and 6 the right.
        const opxy::i32 half = (a4 >> 1) + (a5 >> 1) + (a6 >> 1) + (a7 >> 1);
        d4 += multiply(a4 - half - d4, kSpaceDamp);
        d5 += multiply(a5 - half - d5, kSpaceDamp);
        d6 += multiply(a6 - half - d6, kSpaceDamp);
        d7 += multiply(a7 - half - d7, kSpaceDamp);
        words[(m + kSpaceStart[4]) & kSpaceMask] = space_limit(multiply(d4, job->gain[0]) + xl);
        words[(m + kSpaceStart[5]) & kSpaceMask] = space_limit(multiply(d5, job->gain[1]) + xr);
        words[(m + kSpaceStart[6]) & kSpaceMask] = space_limit(multiply(d6, job->gain[2]) - xl);
        words[(m + kSpaceStart[7]) & kSpaceMask] = space_limit(multiply(d7, job->gain[3]) - xr);
        // Back to full rate, each new sample halfway from the last.
        const opxy::i32 wl = multiply(a4 + a7, job->wet), wr = multiply(a5 + a6, job->wet);
        left[2 * i] = multiply(left[2 * i], job->dry) + (last_left >> 1) + (wl >> 1);
        right[2 * i] = multiply(right[2 * i], job->dry) + (last_right >> 1) + (wr >> 1);
        left[2 * i + 1] = multiply(left[2 * i + 1], job->dry) + wl;
        right[2 * i + 1] = multiply(right[2 * i + 1], job->dry) + wr;
        last_left = wl;
        last_right = wr;
    }
    job->damp[0] = d4;
    job->damp[1] = d5;
    job->damp[2] = d6;
    job->damp[3] = d7;
    job->last[0] = last_left;
    job->last[1] = last_right;
}
inline void space_block(opxy::i32* words, opxy::i32* scratch, SpaceJob* job) {
    OPXY_NATIVE(fragmentedearth_native_space, space_reference)(words, scratch, job);
}
}
