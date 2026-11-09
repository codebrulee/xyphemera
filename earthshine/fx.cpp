#include "core.h"
#include <opxy/q31.hpp>
#include <opxy/q15.hpp>

// Earthshine: a shimmer reverb whose tail is fed back through pitch shifters. The sub
// layer sits an interval under the notes (one octave by default); the up layer
// climbs above them. Light balances the two, from moon (all sub) to sun (all
// up); Shimmer sets how much of either there is.
//
//   in ─► predelay ─┬──────────────────────────────► (+) ─► diffuser ─► figure-eight ─► out
//                   └► sub shifter ► band ─► sub gain ─┤  (4 all-passes,  (two halves,
//   out ─► up shifter ► band ─► up gain ───────────────┘       mono)      Mod drifts each)
//
// The tank runs at half rate (22,050 Hz): it is dark by design, and that halves
// its cost and memory. The shifters splice where the waveforms match
// (correlation-aligned), so they hold pitch where free-running grains drift.
// A send effect: it returns only the wet; Dry keeps the source in the stock mixer.
namespace earthshine {
namespace {
using opxy::i16;
using opxy::i32;
using opxy::u32;
using opxy::q31::add;
using opxy::q31::multiply;
using namespace tables;

const i32 kMaximum = opxy::q31::maximum, kMinimum = -kMaximum - 1;
const u32 kClearPerBlock = 2048;  // words zeroed per block while starting (memory writes are slow)

// The controls, in definition.json order, then the shift row.
// The controls, in definition.json order, then the Shift row: Delay, Swell, Sub, Up.
enum { kDecay, kTone, kLight, kShimmer, kDry, kMod, kFine, kWidth, kDelayChoice, kSwellChoice, kSubInterval, kUpInterval };
const i32 kSubSemitones[3] = {-12, -24, -7};
// The input all-passes' gains, shorter pair first (as Dattorro's input diffusion).
// Character: the level the shimmer is driven at (its average, so a tone's peaks
// reach about 0.95 of the curve: harmonics near -22 dB), and the gain out.
const i32 kDriveLevel = 1288490189;  // 0.6
const i32 kDriveMakeUp = 1234803097;  // 1.15, halved
// The delay's drift: its span (half-rate samples, 3 ms) and ticks a target (1 s).
const i32 kDriftSpan = 66;
const u32 kDriftTicks = 22050;
const i32 kDiffuse1 = 1503238554;  // 0.7: the loop's first diffusers (as -0.7)
const i32 kAllpassGain[kAllpasses] = {1610612736, 1610612736, 1342177280};  // 0.75, 0.75, 0.625

i32 control(i32 word) { return word < 0 ? 0 : word; }
u32 choice(i32 word, u32 count) {
    const u32 index = ((static_cast<u32>(control(word)) >> 16) * count) >> 15;
    return index < count ? index : count - 1;
}

// Saturating left shift by 3, undoing the reads' headroom.

i32 onepole(i32& z, i32 x, i32 a) { return z = add(z, multiply(add(x, -z), a)); }

// Gentle in-loop saturation: x - x^3/4 on the clamped input. Slope 1 at
// silence, so it never changes the decay of a quiet tail.
i32 warm(i32 x) {
    const i32 cube = multiply(multiply(x, x), x);
    return add(x, -(cube >> 2));
}

// 2^f for f in [0, 1), Q16 in, Q30 out (minimax cubic, error about 1e-4).
// 32-bit only: the firmware supplies no 64-bit multiply.
i32 exp2_fraction(i32 f) {
    const i32 c1 = 1492715813, c2 = 485760852, c3 = 167932276;  // 0.6951, 0.2262, 0.0782 in Q31
    const i32 x = f << 15;                                      // Q31
    i32 p = add(c2, multiply(c3, x));
    p = add(c1, multiply(p, x));
    return (1 << 30) + (multiply(p, x) >> 1);  // 1 + (2^f - 1)
}

// 2^-x for x >= 0 in Q16: Q31, as a gain.
i32 gain_for(i32 x) {
    const i32 n = x >> 16, f = x & 0xffff;
    if (n >= 31) return 0;
    if (!f) return n ? 1 << (31 - n) : kMaximum;
    // 2^-x = 2^(1 - f) / 2^(n + 1); 2^(1 - f) is in (1, 2), Q30, so Q31 after >> n.
    return exp2_fraction(65536 - f) >> n;
}

// 2^y for y in Q16 (either sign): Q16, as a pitch ratio.
i32 ratio_for(i32 y) {
    const i32 n = y >> 16;  // floor
    const i32 p = exp2_fraction(y - n * 65536) >> 14;  // Q16, 1..2
    return n >= 0 ? p << n : p >> -n;
}

// A pitch shifter's tap pair: two read ages (Q16 samples), which is playing,
// and the splice in progress.
struct Shifter {
    i32 age[2];
    i32 active;
    i32 left;   // splice samples remaining
    i32 step;   // 1 - ratio, Q16: how the read age moves each sample
};

// Shifter geometry at half rate, by kind: up taps jump back, sub taps forward.
struct Geometry {
    i32 xfade, centre[2], search, match, low, high;
    i32 early;      // samples before low/high its search starts, to finish in time
    i32 fade_step;  // Q31 per splice sample: kMaximum / xfade
    i32 budget;     // its search's work a tick: correlation products and copied samples
};
// Up: 15 ms splice, 33/39 ms jumps +-12 ms, 10 ms match; room for up to +19.
// Sub: 30 ms splice, 64/76 ms jumps +-15 ms, 20 ms match.
const Geometry kUp = {331, {728, 860}, 265, 220, 886, 0, 320, 2147483647 / 331, 18};
// The sub's search is the longest (a 20 ms match over +-15 ms): it starts 560
// samples early, which -24's faster taps still cover after the splice.
const Geometry kSub = {662, {1411, 1676}, 331, 441, 445, 2452, 560, 2147483647 / 662, 13};
// A search's reference and lags must fit the scratch (16-bit samples).
static_assert(2 * 220 + 2 * 265 <= static_cast<int>(tables::kSubScratch), "up search scratch");
static_assert(2 * 441 + 2 * 331 <= static_cast<int>(2 * tables::kScratchWords - tables::kSubScratch), "sub search scratch");

// The splice search in progress (search_start, search_step).
struct Search {
    i32 owner;              // the searching shifter + 1 (0: none)
    i32 geometry;           // its geometry: 0 up, 2 sub
    i32 phase, i;           // kCopying.. and its progress
    u32 t0;                 // the tick it started
    i32 a, up, centre, shift;
    i32 best, best_score, fine_from;
    i32 score_at[11];       // the fine pass's scores (2 kFineHalf + 1)
    i32 lag, k, c, e;       // the lag being scored, paused at sample k with its sums
    i32 jump;               // the result, Q16, once kDone
};

// A gain beyond Q31's range: m (Q31, 0.5..1) times 2^e.
struct Scale {
    i32 m, e;
};

// x times a scale, saturating.
i32 scaled(i32 x, const Scale& s) {
    const i32 y = multiply(x, s.m);
    if (s.e <= 0) return y >> -s.e;
    const i32 limit = kMaximum >> s.e;  // beyond it the shift would overflow
    return y > limit ? kMaximum : y < -limit ? -kMaximum : y << s.e;
}

// The scale num / den (both non-negative Q31), held to 1/64 .. 64.
Scale scale_of(i32 num, i32 den) {
    i32 g = (num >> 1) / ((den >> 13) + 1);  // the ratio x 4096
    if (g < 64) g = 64;
    if (g > 262144) g = 262144;
    Scale s = {0, 0};
    while (g >= 4096) { g >>= 1; ++s.e; }
    while (g < 2048) { g <<= 1; --s.e; }
    s.m = g << 19;
    return s;
}

// The halo's state: its bands' filter states, and this block's coefficients
// (the state-variable filter's a1..a3 and each band's gain, with its fade).
struct Halo {
    u32 phase;                   // where the bands are in their sweep, 2^32 a sweep
    i32 ic1[4], ic2[4];
    i32 a1[4], a2[4], a3[4], gain[4];
};

struct Tank {
    u32 t;                       // half-rate sample count: every line's write position
    i32 down;                    // decimation one-pole, on the input's mono
    i32 held[2][2];              // the last two half-rate wet samples, per side
    i32 damp[2], gain[2];        // per half of the figure-eight
    i32 dc[2];                   // DC blocks on what enters the tank
    u32 lfo[2];
    i32 offset[2];               // the modulated read ages, Q16, gliding each tick
    i32 slide[2];                // per tick, toward the end of the block's
    i32 still;                   // Mod off and every line at its length: plain reads
    i32 decay_word, tone;
    i32 shimmer_level;           // the shimmer's level (for the drive)
    Scale drive_in, drive_out;   // this block's gains into and out of the drive
    i32 sub_gain, up_gain, depth, width, pan;  // smoothed
    u32 pan_phase;               // the autopan's sway, 2^32 a cycle
    i32 pan_left, pan_right;     // this block's gains, Q31
    // Swell: fast and slow followers of the input's level; the shimmer's gain
    // (their ratio, gliding each tick) and the slow one's rise (0: Swell off).
    i32 follow_fast, follow_slow, swell, swell_target, swell_rise;
    u32 shimmer_delay;           // Delay, half-rate samples
    // The delay's human drift: a slow random walk of 0..3 ms, a new target each
    // second (Q16 samples), and its random number.
    i32 drift, drift_slide;
    u32 drift_left, random;
    i32 rate;                    // the LFOs' speed, Q16 (1 to 2.5)
    i32 wet[2];
    i32 heard[2];
    i32 up_high[1], up_low[1], sub_high[1], sub_low[1][2];
    i32 pitch_key;
    Search search[2];            // each shifter's splice search: up, sub
    i32 halo_mode;               // Up: 0 the octave, 1 the halo rising, 2 falling
    Halo halo;
    i32 halo_direct;             // the halo's share heard directly, this tick
    Shifter shifter[2];          // up, sub
};

static_assert(sizeof(Tank) <= kTankWords * 4, "the tank's state fits before the first line");
static_assert(sizeof(State) <= opxy::effect_buffered_state_bytes - 4, "state fits a slot with a shift row");

struct Line {
    i32* words;
    u32 mask;
};

struct Lines {
    Line line[kLines];
    i16* scratch;  // the searches' samples, 12 bits each
};

i32 at(const Line& l, u32 t, u32 age) { return l.words[(t - age) & l.mask]; }

// Linear read, age in Q16 (age >= 1 whole sample).
i32 linear(const Line& l, u32 t, i32 age) {
    const u32 whole = static_cast<u32>(age) >> 16;
    const i32 a = at(l, t, whole) >> 1, b = at(l, t, whole + 1) >> 1;
    return add(a, multiply(b - a, (age & 0xffff) << 15)) * 2;
}


// Bits needed for |v|.
i32 bits(u32 v) {
    i32 n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

// The splice search: the jump (Q16 samples) whose source best matches the
// active tap's next `match` samples, by normalised cross-correlation: coarse
// every 8th lag, then every lag around the best, then a parabola to a fraction
// of a sample. Samples are reduced to 12 bits so the sums fit 32 bits.
//
// Each shifter has its own search, which runs a fixed amount every few ticks
// (its geometry's budget a tick, in correlation products and copied samples:
// enough to finish inside its window), pausing even inside a lag, so no block
// carries more than its share: it starts `early` samples before the tap runs
// out and is done before. The searches are both often running at once, so
// their budgets set the heaviest block: kept to what finishes in time. Ages are taken at the
// tick it starts (t0): the taps keep their spacing, so the jump holds whenever
// it is applied.
enum { kIdle, kCopying, kCoarse, kRefining, kDone };
// The fine pass: lags within kFineHalf of the coarse best (its grid is 8, so
// the peak is within 4), comparing every kFineStep-th sample. The searches run
// every kSearchEvery ticks, that many ticks' budget at once.
const i32 kFineHalf = 5, kFineStep = 4, kSearchEvery = 4;
static_assert(2 * kFineHalf + 1 <= 11, "fine scores");

void search_start(Search& q, const Line& l, u32 t, i32 a, bool up, i32 centre, i32 who, i32 geometry, const Geometry& g) {
    q = Search();
    q.owner = who + 1;
    q.geometry = geometry;
    q.t0 = t;
    q.a = a;
    q.up = up;
    q.centre = centre;
    q.best = centre;
    q.best_score = -1;
    // Scale from the reference's peak.
    u32 peak = 0;
    for (i32 k = 0; k < g.match; k += 4) {
        const i32 v = at(l, t, a - k);
        const u32 mag = v < 0 ? static_cast<u32>(-(v + 1)) + 1 : static_cast<u32>(v);
        if (mag > peak) peak = mag;
    }
    if (!peak) {
        q.jump = centre * 65536;
        q.phase = kDone;
        return;
    }
    q.shift = bits(peak) > 11 ? bits(peak) - 11 : 0;
    q.phase = kCopying;
}

// One lag's products, `n` of them, `step` apart: the reference forward, the
// candidates back. 16 x 16 products, a hardware loop on the Blackfin.
template <i32 kStep>
__attribute__((noinline)) void correlate(const i16* r, const i16* c, i32 n, i32* sums) {
    i32 cs = sums[0], es = sums[1];
    for (i32 i = 0; i < n; ++i) {
        const i32 u = *r, v = *c;
        cs += opxy::q15::product(u, v);
        es += opxy::q15::product(v, v);
        r += kStep;
        c -= kStep;
    }
    sums[0] = cs;
    sums[1] = es;
}

// `n` line samples into the scratch, reduced to 12 bits: from word `index`
// (masked), stepping `dir` (+1 or -1).
__attribute__((noinline)) void gather(i16* out, const i32* words, u32 mask, u32 index, i32 n, i32 dir, i32 shift) {
    for (i32 i = 0; i < n; ++i) {
        i32 v = words[index & mask] >> shift;
        v = v > 2047 ? 2047 : v;
        v = v < -2047 ? -2047 : v;
        out[i] = static_cast<i16>(v);
        index += static_cast<u32>(dir);
    }
}

// A turn of the search (every kSearchEvery ticks): copies, then lags, `budget`
// units' worth.
__attribute__((noinline)) void search_step(Search& q, const Line& l, i16* scratch, const Geometry& g) {
    i32 budget = g.budget * kSearchEvery;
    const i32 m = g.match, span = 2 * g.search + m;
    i16* const ref = scratch;
    i16* const cand = scratch + m;  // cand[idx]: the sample at age (at t0) base + idx
    const i32 base = q.up ? q.a + q.centre - g.search - (m - 1) : q.a - (q.centre + g.search) - (m - 1);
    if (q.phase == kCopying) {
        // The reference (ages a, a - 1, ..: words forward), then the
        // candidates (ages base, base + 1, ..: words back).
        while (q.i < m + span && budget > 0) {
            const bool reference = q.i < m;
            const i32 end = reference ? m : m + span;
            i32 n = budget;
            if (n > end - q.i) n = end - q.i;
            if (reference) gather(ref + q.i, l.words, l.mask, q.t0 - static_cast<u32>(q.a - q.i), n, 1, q.shift);
            else gather(cand + (q.i - m), l.words, l.mask, q.t0 - static_cast<u32>(base + (q.i - m)), n, -1, q.shift);
            q.i += n;
            budget -= n;
        }
        if (q.i < m + span) return;
        q.phase = kCoarse;
        q.i = q.centre - g.search;
        q.k = 0;
    }
    // A lag j's samples: cand[offset(j) - k], k = 0..m-1. Score c^2 / e (c > 0)
    // on reduced sums: c is at most 110 x 2047^2, under 2^29, so c >> 14
    // squared stays under 2^31.
    while (budget > 0 && (q.phase == kCoarse || q.phase == kRefining)) {
        const bool fine = q.phase == kRefining;
        if (q.k == 0) {
            // The next lag, or the next phase.
            const i32 to = fine ? q.fine_from + 2 * kFineHalf : q.centre + g.search;
            if (q.i > to) {
                if (fine) {
                    q.phase = kDone;
                    break;
                }
                q.phase = kRefining;
                q.fine_from = q.best - kFineHalf;
                q.i = q.fine_from;
                q.best_score = -1;
                for (i32 n = 0; n <= 2 * kFineHalf; ++n) q.score_at[n] = -1;
                continue;
            }
            q.lag = q.i;
            q.i += fine ? 1 : 8;
            if (q.lag < q.centre - g.search || q.lag > q.centre + g.search) continue;
            q.c = q.e = 0;
        }
        const i32 j = q.lag;
        const i32 k_step = fine ? kFineStep : 8;
        const i32 offset = q.up ? j - q.centre + g.search + m - 1 : q.centre + g.search - j + m - 1;
        i32 k = q.k;
        // The products left in this lag (k_step is 2 or 8), up to the budget.
        i32 n = (m - k + k_step - 1) >> (fine ? (kFineStep == 2 ? 1 : 2) : 3);
        if (n > budget) n = budget;
        i32 sums[2] = {q.c, q.e};
        if (fine) correlate<kFineStep>(ref + k, cand + offset - k, n, sums);
        else correlate<8>(ref + k, cand + offset - k, n, sums);
        k += n * k_step;
        budget -= n;
        const i32 c = sums[0], e = sums[1];
        if (k < m) {
            // Out of budget mid-lag: resume here next tick.
            q.k = k;
            q.c = c;
            q.e = e;
            break;
        }
        q.k = 0;
        if (c <= 0) continue;
        const i32 cr = c >> 14;
        const i32 score = cr * cr / ((e >> 14) + 1);
        if (fine) q.score_at[j - q.fine_from] = score;
        if (score > q.best_score) {
            q.best_score = score;
            q.best = j;
        }
    }
    if (q.phase != kDone) return;
    // To a fraction of a sample: the vertex of the parabola through the best
    // score and its two neighbours, held within half a sample. Q16.
    q.jump = q.best * 65536;
    const i32 i = q.best - q.fine_from;
    if (q.best_score > 0 && i > 0 && i < 2 * kFineHalf && q.score_at[i - 1] >= 0 && q.score_at[i + 1] >= 0) {
        // Scores reduced below 2^14 so (l - r) * 2^15 fits 32 bits.
        const i32 reduce = bits(static_cast<u32>(q.best_score)) > 14 ? bits(static_cast<u32>(q.best_score)) - 14 : 0;
        const i32 left = q.score_at[i - 1] >> reduce, mid = q.best_score >> reduce, right = q.score_at[i + 1] >> reduce;
        const i32 curve = left - 2 * mid + right;  // negative at a peak
        if (curve < 0) {
            i32 delta = ((left - right) * 32768) / curve;  // Q16: (l - r) / (2 curve)
            if (delta > 32768) delta = 32768;
            if (delta < -32768) delta = -32768;
            q.jump += delta;
        }
    }
}

// One shifter sample: write x, read the playing tap (and the fading one).
// Without `listen`, it keeps its taps in range but skips reads and searches.
// Its search starts `early` samples before its tap runs out (Search).
inline __attribute__((always_inline)) i32 shift(Shifter& s, i32 who, const Line& l, u32 t, i32 x, const Geometry& g, i32 geometry, i32 side, bool listen,
          Search& q) {
    l.words[t & l.mask] = x;
    s.age[0] += s.step;
    s.age[1] += s.step;
    const bool up = s.step < 0;
    const i32 whole = s.age[s.active] >> 16;
    const bool due = up ? whole <= g.low + g.early : whole >= g.high - g.early;
    const bool forced = up ? whole <= g.low : whole >= g.high;
    const bool mine = q.owner == who + 1;
    if (s.left == 0 && due) {
        // Splice when this shifter's search is done; start one if none is running;
        // at the limit, splice regardless: on the search's best so far, or centred.
        i32 jump = -1;
        if (!listen) {
            jump = g.centre[side] * 65536;
        } else if (mine && q.phase == kDone) {
            jump = q.jump;
        } else if (!q.owner) {
            search_start(q, l, t, whole, up, g.centre[side], who, geometry, g);
        }
        if (jump < 0 && forced) jump = mine && q.best_score > 0 ? q.best * 65536 : g.centre[side] * 65536;
        if (jump >= 0) {
            if (mine) q.owner = 0;
            const i32 other = 1 - s.active;
            s.age[other] = s.age[s.active] + (up ? jump : -jump);
            s.active = other;
            s.left = listen ? g.xfade : 0;
        }
    }
    if (!listen) return 0;
    i32 out = linear(l, t, s.age[s.active]);
    if (s.left > 0) {
        // Smoothstep from the old tap to the new.
        const i32 p = (g.xfade - s.left) * g.fade_step;  // Q31, 0..1
        const i32 w = multiply(multiply(p, p), add(0x60000000, -(p >> 1))) * 2;  // p^2 (3 - 2p), /2 then *2
        out = add(multiply(out, w), multiply(linear(l, t, s.age[1 - s.active]),
                                                     kMaximum - w));
        --s.left;
    }
    return out;
}


i32 glide(i32 value, i32 target) {
    const i32 step = (target >> 6) - (value >> 6);
    return step ? value + step : target;
}

// One half-rate sample through the tank.
// Reads and writes on line N, a constant: its mask folds into the code, so
// only its words pointer is loaded.
template <u32 N>
i32 read(const Lines& lines, u32 t, u32 age) { return lines.line[N].words[(t - age) & kLineMask[N]]; }
template <u32 N>
void write(const Lines& lines, u32 t, i32 v) { lines.line[N].words[t & kLineMask[N]] = v; }

// The output taps, unrolled at compile time so each one's line, age and sign
// are constants: a read and an add, where a loop looked each up every tick.
template <u32 N>
struct Taps {
    static i32 left(const Lines& lines, u32 t) {
        const i32 v = read<kTankLine + kTapsLeft[N - 1][0]>(lines, t, kTapsLeft[N - 1][1]) >> 2;
        return add(Taps<N - 1>::left(lines, t), kTapsLeft[N - 1][2] > 0 ? v : -v);
    }
    static i32 right(const Lines& lines, u32 t) {
        const i32 v = read<kTankLine + kTapsRight[N - 1][0]>(lines, t, kTapsRight[N - 1][1]) >> 2;
        return add(Taps<N - 1>::right(lines, t), kTapsRight[N - 1][2] > 0 ? v : -v);
    }
};
template <>
struct Taps<0> {
    static i32 left(const Lines&, u32) { return 0; }
    static i32 right(const Lines&, u32) { return 0; }
};

// The input all-passes, unrolled at compile time like the taps.
template <u32 N>
struct Diffuse {
    static i32 run(const Lines& lines, u32 t, i32 x) {
        x = Diffuse<N - 1>::run(lines, t, x);
        const i32 g = kAllpassGain[N - 1];
        const i32 d = read<kAllpassLine + N - 1>(lines, t, kAllpassLen[N - 1]);
        const i32 w = add(x, -multiply(d, g));
        write<kAllpassLine + N - 1>(lines, t, w);
        return add(d, multiply(w, g));
    }
};
template <>
struct Diffuse<0> {
    static i32 run(const Lines&, u32, i32 x) { return x; }
};

// One half of the figure-eight, its lines and lengths constants: v is the
// input plus the other half's end.
template <u32 H>
void half(Tank& k, const Lines& lines, u32 t, i32 v) {
    const u32 ap1 = kTankLine + H * 4, d1 = ap1 + 1, ap2 = ap1 + 2, d2 = ap1 + 3;
    {
        const i32 d = read<ap1>(lines, t, kAp1[H]);
        const i32 w = add(v, multiply(d, kDiffuse1));  // gain -0.7
        write<ap1>(lines, t, w);
        v = add(d, -multiply(w, kDiffuse1));
    }
    write<d1>(lines, t, warm(v));
    i32 y;
    if (k.still) {
        y = read<d1>(lines, t, kD1[H]);
    } else {
        const i32 age = k.offset[H] += k.slide[H];
        y = linear(lines.line[d1], t, age);
    }
    y = multiply(onepole(k.damp[H], y, k.tone), k.gain[H]);
    {
        const i32 d = read<ap2>(lines, t, kAp2[H]);
        const i32 w = add(y, -(d >> 1));  // gain 0.5
        write<ap2>(lines, t, w);
        y = add(d, w >> 1);
    }
    write<d2>(lines, t, y);
}

// The halo, in place of the octave: four resonant band-passes on the wet's
// middle, a quarter of the range apart, moving together through 250 Hz ..
// 6 kHz (up, or down), each fading in at one end and out at the other, so the
// halo seems to rise (or fall) for ever. Zavalishin's state-variable filter
// (trapezoidal), stable across the whole range at any Q.

// A 65-point table at a Q32 position, interpolated.
i32 across(const i32* table, u32 at) {
    const u32 i = at >> 26, part = (at >> 16) & 1023;
    return table[i] + ((table[i + 1] - table[i]) >> 10) * static_cast<i32>(part);
}

// Once a block: the bands' coefficients where they are, then on a block. The
// halo recirculates without moving its energy out of band (as the octave
// does), so its gain at a band's centre times what recirculates (`level`) is
// held under 0.95: with Decay at full it still dies away.
const i32 kHaloLoop = 255013683;  // 0.95, an eighth
void halo_block(Halo& h, i32 fine, bool falling, i32 level) {
    // Fine across its 33 points (Q31 in: 32 steps of 2^26), interpolated.
    const u32 at = static_cast<u32>(fine) >> 26, part = (static_cast<u32>(fine) >> 16) & 1023;
    const i32 damp = kHaloDamp[at] + ((kHaloDamp[at + 1] - kHaloDamp[at]) >> 10) * static_cast<i32>(part);
    i32 gain = kHaloGain[at] + ((kHaloGain[at + 1] - kHaloGain[at]) >> 10) * static_cast<i32>(part);
    const i32 peak = kHaloPeak[at] + ((kHaloPeak[at + 1] - kHaloPeak[at]) >> 10) * static_cast<i32>(part);
    const i32 loop = multiply(peak, level);  // an eighth
    if (loop > kHaloLoop) gain = multiply(gain, (kHaloLoop / (loop >> 15)) << 16);
    for (u32 b = 0; b < 4; ++b) {
        const u32 p = h.phase + b * 0x40000000u;
        const i32 g = across(kHaloG, falling ? ~p : p);                   // Q30
        const i32 sum = (1 << 29) + multiply(g, add(g, damp >> 1));       // 1 + g (g + 1/Q), Q29
        const i32 a1 = (kMaximum / (sum >> 13)) << 16;                    // Q31
        h.a1[b] = a1;
        h.a2[b] = multiply(g, a1) * 2;
        h.a3[b] = multiply(g, h.a2[b]) * 2;
        h.gain[b] = multiply(gain, across(kHaloFade, p));
    }
    h.phase += static_cast<u32>(static_cast<i32>(kHaloStep[at]) + ((static_cast<i32>(kHaloStep[at + 1]) - static_cast<i32>(kHaloStep[at])) >> 10) * static_cast<i32>(part));
}

// A tick: the input at a 16th (the bands ring up to Q times it), the bands'
// sum back out at 32 times (their gains held at a 32nd).
i32 halo_tick(Halo& h, i32 x) {
    const i32 in = x >> 4;
    i32 out = 0;
    for (u32 b = 0; b < 4; ++b) {
        const i32 v3 = in - h.ic2[b];
        const i32 v1 = add(multiply(h.a1[b], h.ic1[b]), multiply(h.a2[b], v3));
        const i32 v2 = add(add(h.ic2[b], multiply(h.a2[b], h.ic1[b])), multiply(h.a3[b], v3));
        h.ic1[b] = add(v1, v1) - h.ic1[b];
        h.ic2[b] = add(v2, v2) - h.ic2[b];
        out = add(out, multiply(v1, h.gain[b]));
    }
    for (u32 n = 0; n < 5; ++n) out = add(out, out);
    return out;
}

void tick(Tank& k, const Lines& lines, i32 in, bool listen_up, bool listen_sub) {
    const u32 t = ++k.t;
    // The input, in mono (everything after it is: the tank takes a mono
    // input and makes its stereo from its taps), 20 ms late.
    write<kPreLine>(lines, t, in);
    const i32 mono = read<kPreLine>(lines, t, 441);
    // The sub: one shifter on both sides together (it is low enough to be
    // mono), band-limited to 25 Hz .. 2.5 kHz.
    const i32 sub_in = multiply(mono, k.sub_gain);
    // A layer at nothing (Light at its far end) skips its shifter, only
    // keeping its line written for when it returns.
    i32 sub = 0;
    if (listen_sub)
        sub = shift(k.shifter[1], 1, lines.line[kShiftLine + 1], t, add(add(sub_in, sub_in), add(sub_in, sub_in)), kSub, 2, 0, true,
                    k.search[1]);
    else
        write<kShiftLine + 1>(lines, t, 0);
    sub = add(sub, -onepole(k.sub_high[0], sub, kSubHigh));
    sub = onepole(k.sub_low[0][1], onepole(k.sub_low[0][0], sub, kSubLow), kSubLow);
    // The up layer: one shifter on the wet's middle, fed back to both sides
    // (the tank spreads it again), band-limited to 250 Hz .. 6 kHz.
    i32 up = 0;
    if (listen_up && k.halo_mode)
        up = halo_tick(k.halo, add(k.wet[0] >> 1, k.wet[1] >> 1));
    else if (listen_up)
        up = shift(k.shifter[0], 0, lines.line[kShiftLine], t, add(k.wet[0] >> 1, k.wet[1] >> 1),
                   kUp, 0, 0, true, k.search[0]);
    else
        write<kShiftLine>(lines, t, add(k.wet[0] >> 1, k.wet[1] >> 1));
    up = onepole(k.up_low[0], up - onepole(k.up_high[0], up, kUpHigh), kUpLow);
    i32 up_scaled = multiply(up, k.up_gain);
    // The halo mostly goes straight out (at 1.25, matching the octave's
    // level), a quarter of it back into the tank: fed back in full, it piles
    // up whichever of the tank's resonances a band lingers on into one tone.
    k.halo_direct = 0;
    if (k.halo_mode) {
        const i32 direct = multiply(up_scaled, k.swell);
        k.halo_direct = add(direct, direct >> 2);
        up_scaled >>= 2;
    }

    // Swell: the shimmer comes in as the slow follower catches the fast one
    // after each note; Delay then holds it back before it reaches the tank.
    const i32 level = mono < 0 ? -(mono + 1) : mono;
    // The level, smoothed over about 10 ms (so the followers see the note, not
    // each cycle); the slow follower rises to it over Swell's time and falls
    // with it, so it catches up fully once a note holds.
    k.follow_fast = add(k.follow_fast, multiply(level - k.follow_fast, kFollowFast));
    k.follow_slow = k.swell_rise ? add(k.follow_slow, multiply(k.follow_fast - k.follow_slow,
                                                               k.follow_fast > k.follow_slow ? k.swell_rise : kFollowFall))
                                 : k.follow_fast;
    k.swell = add(k.swell, (k.swell_target >> 8) - (k.swell >> 8));  // about 12 ms: no ripple per block
    i32 shimmer = multiply(add(up_scaled, sub), k.swell);
    // Character: the shimmer brought to one level (kDriveLevel), driven into a
    // soft, slightly lopsided curve (the tank's warm, plus a little second
    // harmonic, as a valve stage), then returned to its own level. The same
    // character however loud it is; the tank's input DC block takes out the
    // second harmonic's offset.
    {
        k.shimmer_level = add(k.shimmer_level, multiply((shimmer < 0 ? -(shimmer + 1) : shimmer) - k.shimmer_level, kFollowFast));
        i32 driven = scaled(shimmer, k.drive_in);
        driven = add(warm(driven), multiply(driven, driven) >> 3);
        shimmer = scaled(driven, k.drive_out);
    }
    {
        // Delay, drifting by up to 3 ms so the shimmer's timing breathes like a
        // player's. Moving a delay bends its pitch a touch: a second a target
        // keeps that under about 5 cents.
        if (!k.drift_left) {
            k.random = k.random * 1664525u + 1013904223u;
            const i32 target = static_cast<i32>(k.random >> 16) * kDriftSpan;  // Q16 samples, 0..span
            k.drift_slide = (target - k.drift) / static_cast<i32>(kDriftTicks);
            k.drift_left = kDriftTicks;
        }
        k.drift += k.drift_slide;
        --k.drift_left;
        const Line& l = lines.line[kShimmerLine];
        l.words[t & l.mask] = shimmer;
        const i32 age = static_cast<i32>((k.shimmer_delay + 2) << 16) + k.drift;
        shimmer = linear(l, t, age);
    }

    // The figure-eight (Dattorro's plate, Griesinger's topology): the input,
    // in mono, through three all-passes, then into both halves of one loop.
    // Each half: a decay diffuser (-0.7), a delay Mod drifts, damping and the
    // decay's gain, a second diffuser (0.5), a delay; its end feeds the other.
    i32 x = add(mono, shimmer);
    x = add(x, -onepole(k.dc[0], x, kDcBlock));
    x = Diffuse<kAllpasses>::run(lines, t, x);
    const i32 ends[2] = {read<kTankLine + 3>(lines, t, kD2[0]), read<kTankLine + 7>(lines, t, kD2[1])};
    half<0>(k, lines, t, add(x, ends[1]));
    half<1>(k, lines, t, add(x, ends[0]));
    // Out: Dattorro's taps, five a side, mostly from the other half.
    i32 out[2] = {Taps<kTaps>::left(lines, t), Taps<kTaps>::right(lines, t)};
    for (u32 ch = 0; ch < 2; ++ch) out[ch] = add(add(out[ch], out[ch]), multiply(out[ch], 347892350));  // x 2.16
    k.wet[0] = out[0];
    k.wet[1] = out[1];
    {
        const i32 mid = add(add(out[0] >> 1, out[1] >> 1), k.halo_direct), side = multiply(add(out[0] >> 1, -(out[1] >> 1)), k.width);
        k.heard[0] = multiply(add(mid, side), k.pan_left);
        k.heard[1] = multiply(add(mid, -side), k.pan_right);
    }
}


// Every kModTicks ticks (a quarter block): the step that glides each half's
// modulated read age, a tick at a time, to its length plus a slow parabolic-sine drift
// of 0..depth by the segment's end. Steering it this often keeps the bends in
// its path too small to hear as a buzz.
const u32 kModTicks = 8;
void modulate(Tank& k) {
    for (u32 c = 0; c < 2; ++c) {
        const i32 phase = static_cast<i32>(k.lfo[c]);
        const i32 magnitude = phase < 0 ? -(phase + 1) : phase;
        const i32 wave = multiply(phase, kMaximum - magnitude) * 4;  // -1..1
        k.lfo[c] += ((kLfoStep[c] * kModTicks) >> 8) * static_cast<u32>(k.rate >> 8);
        const i32 target = static_cast<i32>(kD1[c] << 16) + multiply(k.depth, (wave >> 1) + 0x40000000);
        k.slide[c] = (target - k.offset[c]) / static_cast<i32>(kModTicks);
    }
    // Still once Mod is off and every line has glided back to its length.
    k.still = !k.depth;
    for (u32 c = 0; c < 2; ++c)
        if (k.slide[c] || k.offset[c] != static_cast<i32>(kD1[c] << 16)) k.still = 0;
}

void setup(Tank& k) {
    k = Tank();
    for (u32 c = 0; c < 2; ++c) k.lfo[c] = kLfoPhase[c];
    k.decay_word = -1;
    k.pitch_key = -1;
    for (u32 c = 0; c < 2; ++c) k.offset[c] = static_cast<i32>(kD1[c] << 16);
    k.width = -1;  // none yet: the first block takes the control as it is
    k.pan = -1;
    k.swell = k.swell_target = kMaximum;
    k.rate = 65536;
    k.shifter[0].age[0] = k.shifter[0].age[1] = (kUp.low + kUp.centre[0]) << 16;
    k.shifter[1].age[0] = k.shifter[1].age[1] = (kSub.low + kSub.centre[0] / 2) << 16;
}

// Per block: the controls' targets.
void controls(Tank& k, opxy::Parameters p) {
    // Decay: RT60 from 1 to 20 s, exponential. log2 gain = -K * 2^(-v log2 20).
    if (p[kDecay] != k.decay_word) {
        k.decay_word = p[kDecay];
        const i32 per_second = gain_for(multiply(control(p[kDecay]), 283234));  // 2^(-v log2 20), Q31
        for (u32 c = 0; c < 2; ++c) k.gain[c] = gain_for(multiply(kDecayK[c], per_second));
    }
    // Tone: the damping table, interpolated.
    const u32 tone = static_cast<u32>(control(p[kTone])) >> 16;  // Q15
    const u32 at_index = tone >> 10, part = tone & 1023;
    const i32 target_tone = at_index >= 32 ? kToneCoefficient[32]
                                           : kToneCoefficient[at_index] + ((kToneCoefficient[at_index + 1] - kToneCoefficient[at_index]) >> 10) * static_cast<i32>(part);
    k.tone = target_tone;
    // Mod: up to 10 ms (220 samples) of drift, Q16, on a squared curve so its
    // first half covers the gentle range; the LFOs speed up to 2.5 times as fast.
    const i32 mod = control(p[kMod]);
    k.depth = glide(k.depth, multiply(multiply(mod, mod), 220 << 16));
    k.rate = 65536 + ((mod >> 15) * 3 >> 1);
    // Width: up to 70% the sides open from mono to the tank's full spread; above
    // it they stay full and a slow autopan (a sway about every 5 s) comes in,
    // on a squared curve: subtle at first, pronounced at the top, where the far
    // side dips by up to 8 dB. It only ever turns a side down, so it never clips.
    const i32 width = control(p[kWidth]), kOpen = 1503238554;  // 0.7
    const i32 sides = width >= kOpen ? kMaximum : add(multiply(width, 1533916891), multiply(width, 1533916891));  // width / 0.7
    i32 sway = 0;
    if (width > kOpen) {
        const i32 quarter = multiply(width - kOpen, 1789569707);  // (width - 0.7) / 0.3, quartered
        const i32 linear = add(add(quarter, quarter), add(quarter, quarter));
        sway = multiply(linear, linear);
    }
    k.width = k.width < 0 ? sides : glide(k.width, sides);
    k.pan = k.pan < 0 ? sway : glide(k.pan, sway);
    k.pan_phase += 1246615;  // 0.2 Hz: 2^32 x 0.2 x 64 / 44,100 a block
    const i32 phase = static_cast<i32>(k.pan_phase);
    const i32 magnitude = phase < 0 ? -(phase + 1) : phase;
    const i32 wave = multiply(phase, kMaximum - magnitude) * 4;  // a parabolic sine, -1..1
    const i32 dip = multiply(multiply(k.pan, wave < 0 ? -wave : wave), 1288490189);  // up to 0.6 (8 dB)
    k.pan_left = wave > 0 ? kMaximum - dip : kMaximum;
    k.pan_right = wave < 0 ? kMaximum - dip : kMaximum;
    // Pitch: the intervals, with Fine stretching them (+-25 cents).
    k.shimmer_delay = kShimmerDelay[choice(p[kDelayChoice], 4)];
    k.swell_rise = kSwellRise[choice(p[kSwellChoice], 4)];
    // The swell's gain: slow over fast, squared, at most 1 (Q15 from their top bits).
    if (!k.swell_rise || k.follow_fast < (1 << 16)) {
        k.swell_target = kMaximum;
    } else {
        const i32 ratio = ((k.follow_slow >> 16) << 15) / ((k.follow_fast >> 16) + 1);
        const i32 linear = ratio >= 32768 ? kMaximum : ratio << 16;
        k.swell_target = multiply(linear, linear);  // squared: a quieter start, a longer rise
    }
    // The drive's gains: into the curve at kDriveLevel, and back out.
    {
        const i32 level = k.shimmer_level > 4096 ? k.shimmer_level : 4096;
        k.drive_in = scale_of(kDriveLevel, level);
        k.drive_out = scale_of(multiply(level, kDriveMakeUp), kDriveLevel >> 1);
    }
    const u32 sub = choice(p[kSubInterval], 3);
    k.halo_mode = static_cast<i32>(choice(p[kUpInterval], 3));
    // Fine: with the octave, it stretches the intervals (rounded, so the centre
    // is exactly 0 cents); with a halo, it sets the halo's character and the
    // sub stays exact.
    const i32 fine = control(p[kFine]);
    const i32 cents = k.halo_mode ? 0 : ((static_cast<i32>(static_cast<u32>(fine) >> 16) - 16384) * 25 + 8192) >> 14;
    const i32 key = static_cast<i32>(sub) * 256 + cents + 128;
    if (key != k.pitch_key) {
        k.pitch_key = key;
        k.shifter[0].step = 65536 - ratio_for(((1200 + cents) * 65536) / 1200);
        k.shifter[1].step = 65536 - ratio_for(((kSubSemitones[sub] * 100 - cents) * 65536) / 1200);
    }
}
}

void initialize(State& state) {
    // An engine reset runs this again: the buffer is cleared again before use.
    state.cleared = 0;
    state.ready = 0;
}

void process(State& state, opxy::Buffer buffer, opxy::Channel left, opxy::Channel right,
             opxy::Parameters parameters) {
    if (!buffer.words || buffer.count < kBufferWords) {
        for (unsigned i = 0; i < opxy::block_frames; ++i) left[i] = right[i] = 0;
        return;
    }
    // Starting: zero the buffer a slice per block, silent meanwhile.
    if (state.cleared < kBufferWords) {
        const u32 end = state.cleared + kClearPerBlock < kBufferWords ? state.cleared + kClearPerBlock : kBufferWords;
        for (u32 i = state.cleared; i < end; ++i) buffer.words[i] = 0;
        state.cleared = end;
        for (unsigned i = 0; i < opxy::block_frames; ++i) left[i] = right[i] = 0;
        return;
    }
    Tank& stored = *reinterpret_cast<Tank*>(buffer.words);
    if (!state.ready) {
        setup(stored);
        state.ready = 1;
    }
    // The tank's state, worked on where it lives (copying it in and out
    // each block costs more than it saves).
    Tank& k = stored;
    Lines lines;
    for (u32 n = 0; n < kLines; ++n) lines.line[n] = Line{buffer.words + kLineOffset[n], kLineMask[n]};
    lines.scratch = reinterpret_cast<i16*>(buffer.words + kScratch);

    controls(k, parameters);
    // Light balances moon (sub) and sun (up); Shimmer scales both. Its last
    // 5% at each end turns the other layer fully off, and its shifter rests:
    // 0.05 .. 0.95 spreads over 0 .. 1.
    const i32 amount = control(parameters[kShimmer]);
    const i32 raw = control(parameters[kLight]) - 107374182;  // less 0.05
    const i32 light = raw <= 0 ? 0 : raw >= 1932735283 ? kMaximum : add(raw, multiply(raw, 238609294));  // x 1/0.9
    // The sub: 1.5 x its share, plus as much again cubed, so it reaches twice
    // that (+6 dB) at the top while low settings stay close. Held at a quarter
    // here (up to 0.75) and applied x4.
    const i32 sub_share = multiply(amount, kMaximum - light);
    const i32 sub_target = multiply(add(sub_share, multiply(multiply(sub_share, sub_share), sub_share)), 0x30000000);
    const i32 up_target = multiply(multiply(amount, light), 921708850);               // up to 0.43
    const bool listen_up = up_target > 0 || k.up_gain > 0;
    if (k.halo_mode) halo_block(k.halo, control(parameters[kFine]), k.halo_mode == 2, up_target >> 2);  // a quarter recirculates
    const bool listen_sub = sub_target > 0 || k.sub_gain > 0;

    // Frames in pairs (a block is a whole number of them): each pair's first
    // is decimated and output, then its second runs the tick at half rate.
    for (unsigned i = 0; i < opxy::block_frames; i += 2) {
        // Decimate: a one-pole at 8 kHz, then every other sample.
        onepole(k.down, add(left[i] >> 1, right[i] >> 1), kDecimate);
        // Back to full rate: halfway between the last two, then the latest
        // (the tank is dark, so linear interpolation is clean enough).
        left[i] = add(k.held[0][0] >> 1, k.held[0][1] >> 1);
        right[i] = add(k.held[1][0] >> 1, k.held[1][1] >> 1);
        const i32 in = onepole(k.down, add(left[i + 1] >> 1, right[i + 1] >> 1), kDecimate);
        k.sub_gain = glide(k.sub_gain, sub_target);
        k.up_gain = glide(k.up_gain, up_target);
        if ((k.t & (kModTicks - 1)) == 0) modulate(k);
        tick(k, lines, in, listen_up, listen_sub);
        // The splice searches in progress, a tick's worth each (here, between
        // ticks, where little else is held in registers).
        if ((k.t & (kSearchEvery - 1)) == 0 && k.search[0].owner && k.search[0].phase != kDone)
            search_step(k.search[0], lines.line[kShiftLine], lines.scratch, kUp);
        if ((k.t & (kSearchEvery - 1)) == 0 && k.search[1].owner && k.search[1].phase != kDone)
            search_step(k.search[1], lines.line[kShiftLine + 1], lines.scratch + kSubScratch, kSub);
        for (i32 ch = 0; ch < 2; ++ch) {
            k.held[ch][0] = k.held[ch][1];
            k.held[ch][1] = k.heard[ch];
        }
        left[i + 1] = k.held[0][1];
        right[i + 1] = k.held[1][1];
    }
}
}
