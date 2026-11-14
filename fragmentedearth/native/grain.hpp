#pragma once
#include <opxy/native.hpp>
#include <opxy/q31.hpp>
#include <opxy/types.hpp>

// One grain's stretch of samples (fx.cpp's play(), where the whole stretch lies inside its
// window and the recording): read with linear interpolation, then mixed into the output at a
// gain ramping by a step a sample. The device runs native/grain.s.
namespace fragmentedearth {
struct GrainJob {
    opxy::u32 position;          // Q14 samples into the window; returned where it ends
    opxy::i32 rate;              // Q14 samples a sample
    opxy::u32 count;             // samples, 1..64
    opxy::i32 left, step_left;   // the gains (Q31) and their steps a sample
    opxy::i32 right, step_right;
    opxy::i32* out_right;        // added to, as out_left
    opxy::i32* scratch;          // count words
};
}

extern "C" void fragmentedearth_native_grain(const opxy::i16* recording, opxy::i32* out_left, fragmentedearth::GrainJob* job);

namespace fragmentedearth {
}


namespace fragmentedearth {
// A recorded sample back to Q29 (Q23 x 64), so two samples' difference still fits: its
// mantissa (the top 12 bits) shifted up by its exponent (the low 4) and 6.
inline opxy::i32 decode(opxy::i16 c) {
    const opxy::i32 v = c;
    return (v >> 4) << ((static_cast<opxy::u32>(v) << 28 >> 28) + 6);
}
// The definition: `recording` is where the window begins.
inline void grain_reference(const opxy::i16* recording, opxy::i32* out_left, GrainJob* job) {
    using opxy::q31::multiply;
    opxy::u32 position = job->position;
    const opxy::u32 rate = static_cast<opxy::u32>(job->rate), count = job->count;
    opxy::i32* v = job->scratch;
    for (opxy::u32 i = 0; i < count; ++i) {
        const opxy::i32 a = decode(recording[position >> 14]), b = decode(recording[(position >> 14) + 1]);
        v[i] = (a + multiply(b - a, static_cast<opxy::i32>(position << 18 >> 1))) << 2;  // (the fraction, Q31)
        position += rate;
    }
    opxy::i32 left = job->left, right = job->right;
    for (opxy::u32 i = 0; i < job->count; ++i) {
        out_left[i] += multiply(v[i], left) >> 5;
        job->out_right[i] += multiply(v[i], right) >> 5;
        left += job->step_left;
        right += job->step_right;
    }
    job->position = position;
}
inline void grain(const opxy::i16* recording, opxy::i32* out_left, GrainJob* job) {
    OPXY_NATIVE(fragmentedearth_native_grain, grain_reference)(recording, out_left, job);
}
}
