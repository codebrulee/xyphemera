#include "core.h"
#include <opxy/q31.hpp>

// Earthshine: a shimmer reverb whose tail is fed back through pitch shifters. The sub
// layer sits an interval under the notes (one octave by default); the up layer
// climbs above them. Light balances the two, from moon (all sub) to sun (all
// up); Shimmer sets how much of either there is.
//
//   in ─► predelay ─┬──────────────────────────────► (+) ─► diffuser ─► FDN ─► out
//                   └► sub shifter ► band ─► sub gain ─┤  (4 all-passes  (4 lines,
//   out ─► up shifter ► band ─► up gain ───────────────┘     per side)    modulated)
//
// The tank runs at half rate (22,050 Hz): it is dark by design, and that halves
// its cost and memory. The shifters splice where the waveforms match
// (correlation-aligned), so they hold pitch where free-running grains drift.
// A send effect: it returns only the wet; Dry keeps the source in the stock mixer.
namespace earthshine {
namespace {
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
const i32 kUpSemitones[3] = {12, 7, 19};
// The input all-passes' gains, shorter pair first (as Dattorro's input diffusion).
const i32 kAllpassGain[4] = {1610612736, 1610612736, 1342177280, 1342177280};  // 0.75, 0.75, 0.625, 0.625

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
    i32 early;      // samples before low/high a splice may start, to wait its turn
    i32 fade_step;  // Q31 per splice sample: kMaximum / xfade
};
// Up: 15 ms splice, 33/39 ms jumps +-12 ms, 10 ms match; room for up to +19.
// Sub: 30 ms splice, 64/76 ms jumps +-15 ms, 20 ms match.
const Geometry kUp = {331, {728, 860}, 265, 220, 886, 0, 256, 2147483647 / 331};
// Up an octave and a fifth: the taps move twice as fast, so the jumps are
// doubled to keep each grain well beyond its splice.
const Geometry kUpWide = {331, {1456, 1720}, 265, 220, 886, 0, 256, 2147483647 / 331};
const Geometry kSub = {662, {1411, 1676}, 331, 441, 445, 2452, 128, 2147483647 / 662};
// A search's reference and lags must fit the scratch.
static_assert(2 * 441 + 2 * 331 <= static_cast<int>(tables::kScratchWords), "sub search scratch");
static_assert(2 * 220 + 2 * 265 <= static_cast<int>(tables::kScratchWords), "up search scratch");

// The splice search in progress (search_start, search_step).
struct Search {
    i32 owner;              // the searching shifter + 1 (0: none)
    i32 geometry;           // its geometry: 0 up, 1 wide up, 2 sub
    i32 phase, i;           // kCopying.. and its progress
    u32 t0;                 // the tick it started
    i32 a, up, centre, shift;
    i32 best, best_score, fine_from;
    i32 score_at[15];
    i32 jump;               // the result, Q16, once kDone
};

struct Tank {
    u32 t;                       // half-rate sample count: every line's write position
    i32 odd;                     // decimation phase
    i32 down[2];                 // decimation one-pole, per side
    i32 held[2][2];              // the last two half-rate wet samples, per side
    i32 smooth[2];               // interpolation one-pole, per side
    i32 damp[4], gain[4];
    i32 dc[2];                   // DC blocks on what enters the tank
    u32 lfo[4];
    i32 offset[4];               // the modulated read ages, Q16, gliding each tick
    i32 slide[4];                // per tick, toward the end of the block's
    i32 decay_word, tone;
    i32 sub_gain, up_gain, depth, width, pan;  // smoothed
    u32 pan_phase;               // the autopan's sway, 2^32 a cycle
    i32 pan_left, pan_right;     // this block's gains, Q31
    // Swell: fast and slow followers of the input's level; the shimmer's gain
    // (their ratio, gliding each tick) and the slow one's rise (0: Swell off).
    i32 follow_fast, follow_slow, swell, swell_target, swell_rise;
    u32 shimmer_delay;           // Delay, half-rate samples
    i32 rate;                    // the LFOs' speed, Q16 (1 to 2.5)
    i32 wet[2];
    i32 up_high[1], up_low[1], sub_high[1], sub_low[1][2];
    i32 pitch_key;
    Search search;               // one splice search at a time
    i32 up_wide;                 // the up interval is +19: kUpWide
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
    i32* scratch;
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
// One search runs at a time, a little each tick (kSearchBudget products), so no
// block carries a whole search: it starts `early` samples before the tap runs
// out and is done well before. Ages are taken at the tick it starts (t0): the
// taps keep their spacing, so the jump holds whenever it is applied.
const i32 kSearchBudget = 64;  // correlation products a tick
enum { kIdle, kCopying, kCoarse, kRefining, kDone };

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

// One tick of the search: copies, then lags, up to kSearchBudget products.
void search_step(Search& q, const Line& l, i32* scratch, const Geometry& g) {
    const i32 m = g.match, span = 2 * g.search + m;
    i32* const ref = scratch;
    i32* const cand = scratch + m;  // cand[idx]: the sample at age (at t0) base + idx
    const i32 base = q.up ? q.a + q.centre - g.search - (m - 1) : q.a - (q.centre + g.search) - (m - 1);
    i32 budget = kSearchBudget;
    if (q.phase == kCopying) {
        // Reference, then candidates, four samples a product's worth.
        for (; q.i < m + span && budget > 0; ++q.i) {
            const i32 age = q.i < m ? q.a - q.i : base + (q.i - m);
            i32 v = at(l, q.t0, age) >> q.shift;
            v = v > 2047 ? 2047 : v < -2047 ? -2047 : v;
            if (q.i < m) ref[q.i] = v;
            else cand[q.i - m] = v;
            if ((q.i & 3) == 3) --budget;
        }
        if (q.i < m + span) return;
        q.phase = kCoarse;
        q.i = q.centre - g.search;
    }
    // A lag j's samples: cand[offset(j) - k], k = 0..m-1. Score c^2 / e (c > 0)
    // on reduced sums: c is at most 110 x 2047^2, under 2^29, so c >> 14
    // squared stays under 2^31.
    while (budget > 0 && (q.phase == kCoarse || q.phase == kRefining)) {
        const bool fine = q.phase == kRefining;
        const i32 to = fine ? q.fine_from + 14 : q.centre + g.search;
        if (q.i > to) {
            if (fine) {
                q.phase = kDone;
                break;
            }
            q.phase = kRefining;
            q.fine_from = q.best - 7;
            q.i = q.fine_from;
            q.best_score = -1;
            for (i32 n = 0; n < 15; ++n) q.score_at[n] = -1;
            continue;
        }
        const i32 j = q.i;
        q.i += fine ? 1 : 8;
        if (j < q.centre - g.search || j > q.centre + g.search) continue;
        const i32 k_step = fine ? 2 : 8;
        const i32 offset = q.up ? j - q.centre + g.search + m - 1 : q.centre + g.search - j + m - 1;
        i32 c = 0, e = 0;
        for (i32 k = 0; k < m; k += k_step) {
            const i32 u = ref[k], v = cand[offset - k];
            c += u * v;
            e += v * v;
        }
        budget -= m / k_step;
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
    if (q.best_score > 0 && i > 0 && i < 14 && q.score_at[i - 1] >= 0 && q.score_at[i + 1] >= 0) {
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
i32 shift(Shifter& s, i32 who, const Line& l, u32 t, i32 x, const Geometry& g, i32 geometry, i32 side, bool listen,
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

void householder(i32* x) {
    i32 sum = 0;
    for (u32 i = 0; i < 4; ++i) sum = add(sum, x[i] >> 1);  // 2/4 of the sum
    for (u32 i = 0; i < 4; ++i) x[i] = add(x[i], -sum);
}

i32 glide(i32 value, i32 target) {
    const i32 step = (target >> 6) - (value >> 6);
    return step ? value + step : target;
}

// One half-rate sample through the tank.
void tick(Tank& k, const Lines& lines, i32 in_left, i32 in_right, bool listen_up, bool listen_sub) {
    const u32 t = ++k.t;
    const i32 in[2] = {in_left, in_right};
    i32 delayed[2];
    for (i32 ch = 0; ch < 2; ++ch) {
        const Line& pre = lines.line[kPreLine + ch];
        pre.words[t & pre.mask] = in[ch];
        delayed[ch] = at(pre, t, 441);  // 20 ms
    }
    // The sub: one shifter on both sides together (it is low enough to be
    // mono), band-limited to 25 Hz .. 2.5 kHz.
    const i32 sub_in = multiply(add(delayed[0] >> 1, delayed[1] >> 1), k.sub_gain);
    i32 sub = shift(k.shifter[1], 1, lines.line[kShiftLine + 1], t, add(add(sub_in, sub_in), add(sub_in, sub_in)), kSub, 2, 0, listen_sub,
                    k.search);
    sub = add(sub, -onepole(k.sub_high[0], sub, kSubHigh));
    sub = onepole(k.sub_low[0][1], onepole(k.sub_low[0][0], sub, kSubLow), kSubLow);
    // The up layer: one shifter on the wet's middle, fed back to both sides
    // (the tank spreads it again), band-limited to 250 Hz .. 6 kHz.
    i32 up = shift(k.shifter[0], 0, lines.line[kShiftLine], t, add(k.wet[0] >> 1, k.wet[1] >> 1),
                   k.up_wide ? kUpWide : kUp, k.up_wide ? 1 : 0, 0, listen_up, k.search);
    // The splice search in progress, a tick's worth.
    if (k.search.owner && k.search.phase != kDone) {
        const Geometry& g = k.search.geometry == 2 ? kSub : k.search.geometry == 1 ? kUpWide : kUp;
        search_step(k.search, lines.line[kShiftLine + k.search.owner - 1], lines.scratch, g);
    }
    up = onepole(k.up_low[0], up - onepole(k.up_high[0], up, kUpHigh), kUpLow);
    const i32 up_scaled = multiply(up, k.up_gain);

    // Swell: the shimmer comes in as the slow follower catches the fast one
    // after each note; Delay then holds it back before it reaches the tank.
    const i32 mono = add(delayed[0] >> 1, delayed[1] >> 1);
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
    if (k.shimmer_delay) {
        const Line& l = lines.line[kShimmerLine];
        l.words[t & l.mask] = shimmer;
        shimmer = l.words[(t - k.shimmer_delay) & l.mask];
    }
    i32 inject[2];
    for (i32 ch = 0; ch < 2; ++ch) inject[ch] = add(delayed[ch], shimmer);

    // Spread the two sides over eight channels, then diffuse.
    for (i32 ch = 0; ch < 2; ++ch) {
        i32 v = add(inject[ch], -onepole(k.dc[ch], inject[ch], kDcBlock));
        for (u32 s = 0; s < 4; ++s) {
            const Line& l = lines.line[kAllpassLine + ch * 4 + s];
            const i32 g = kAllpassGain[s];
            const i32 d = at(l, t, kAllpassLen[ch][s]);
            const i32 w = add(v, -multiply(d, g));
            l.words[t & l.mask] = w;
            v = add(d, multiply(w, g));
        }
        inject[ch] = v;
    }
    // The FDN, four lines, each read between two samples with its own slow drift.
    i32 y[4], f[4];
    for (u32 c = 0; c < 4; ++c) {
        const Line& l = lines.line[kFdnLine + c];
        const i32 age = k.offset[c] += k.slide[c];
        const u32 at_age = t - (static_cast<u32>(age) >> 16);
        const i32 a0 = l.words[at_age & l.mask] >> 1, a1 = l.words[(at_age - 1) & l.mask] >> 1;
        y[c] = f[c] = add(a0, multiply(a1 - a0, (age & 0xffff) << 15)) * 2;
    }
    householder(f);
    // Back in: each line's damped, scaled, warmed signal, plus the diffused input
    // (left into 0 and 2, right into 1 and 3, the second pair inverted).
    const i32 in_half[2] = {inject[0] >> 1, inject[1] >> 1};
    for (u32 c = 0; c < 4; ++c) {
        const i32 v = multiply(onepole(k.damp[c], f[c], k.tone), k.gain[c]);
        const Line& l = lines.line[kFdnLine + c];
        l.words[t & l.mask] = add(warm(v), (c & 2) ? -in_half[c & 1] : in_half[c & 1]);
    }
    // Out: each line's end plus three taps along it, signs alternating so the
    // sides decorrelate; left from lines 0 and 2, right from 1 and 3; then
    // +2.5 dB, the level the longer lines spread thinner.
    i32 out[2] = {0, 0};
    for (u32 c = 0; c < 4; ++c) {
        const Line& l = lines.line[kFdnLine + c];
        i32 sum = y[c] >> 1;
        sum = add(sum, l.words[(t - kTap[c][0]) & l.mask] >> 1);
        sum = add(sum, -(l.words[(t - kTap[c][1]) & l.mask] >> 1));
        sum = add(sum, l.words[(t - kTap[c][2]) & l.mask] >> 1);
        out[c & 1] = add(out[c & 1], (c & 2) ? -sum : sum);
    }
    for (u32 ch = 0; ch < 2; ++ch) out[ch] = add(out[ch], multiply(out[ch], 718916480));  // x 1.335
    k.wet[0] = out[0];
    k.wet[1] = out[1];
}


// Every kModTicks ticks (a quarter block): the step that glides each FDN line's
// read age, a tick at a time, to its length plus a slow parabolic-sine drift
// of 0..depth by the segment's end. Steering it this often keeps the bends in
// its path too small to hear as a buzz.
const u32 kModTicks = 8;
void modulate(Tank& k) {
    for (u32 c = 0; c < 4; ++c) {
        const i32 phase = static_cast<i32>(k.lfo[c]);
        const i32 magnitude = phase < 0 ? -(phase + 1) : phase;
        const i32 wave = multiply(phase, kMaximum - magnitude) * 4;  // -1..1
        k.lfo[c] += ((kLfoStep[c] * kModTicks) >> 8) * static_cast<u32>(k.rate >> 8);
        const i32 target = static_cast<i32>(kFdnLen[c] << 16) + multiply(k.depth, (wave >> 1) + 0x40000000);
        k.slide[c] = (target - k.offset[c]) / static_cast<i32>(kModTicks);
    }
}

void setup(Tank& k) {
    k = Tank();
    for (u32 c = 0; c < 4; ++c) k.lfo[c] = kLfoPhase[c];
    k.decay_word = -1;
    k.pitch_key = -1;
    for (u32 c = 0; c < 4; ++c) k.offset[c] = static_cast<i32>(kFdnLen[c] << 16);
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
        for (u32 c = 0; c < 4; ++c) k.gain[c] = gain_for(multiply(kDecayK[c], per_second));
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
    const u32 sub = choice(p[kSubInterval], 3), up = choice(p[kUpInterval], 3);
    k.up_wide = up == 2;
    // Rounded, so the centre is exactly 0 cents.
    const i32 cents = ((static_cast<i32>(static_cast<u32>(control(p[kFine])) >> 16) - 16384) * 25 + 8192) >> 14;
    const i32 key = static_cast<i32>(sub * 3 + up) * 256 + cents + 128;
    if (key != k.pitch_key) {
        k.pitch_key = key;
        k.shifter[0].step = 65536 - ratio_for(((kUpSemitones[up] * 100 + cents) * 65536) / 1200);
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
    // The tank's state is worked on in a local copy (fast memory), then stored.
    Tank k = stored;
    Lines lines;
    for (u32 n = 0; n < kLines; ++n) lines.line[n] = Line{buffer.words + kLineOffset[n], kLineMask[n]};
    lines.scratch = buffer.words + kScratch;

    controls(k, parameters);
    // Light balances moon (sub) and sun (up); Shimmer scales both.
    const i32 amount = control(parameters[kShimmer]), light = control(parameters[kLight]);
    // The sub: 1.5 x its share, plus as much again cubed, so it reaches twice
    // that (+6 dB) at the top while low settings stay close. Held at a quarter
    // here (up to 0.75) and applied x4.
    const i32 sub_share = multiply(amount, kMaximum - light);
    const i32 sub_target = multiply(add(sub_share, multiply(multiply(sub_share, sub_share), sub_share)), 0x30000000);
    const i32 up_target = multiply(multiply(amount, light), 0x4ccccccd);              // up to 0.6
    const bool listen_up = up_target > 0 || k.up_gain > 0;
    const bool listen_sub = sub_target > 0 || k.sub_gain > 0;

    for (unsigned i = 0; i < opxy::block_frames; ++i) {
        // Decimate: a one-pole at 8 kHz, then every other sample.
        const i32 l = onepole(k.down[0], left[i], kDecimate);
        const i32 r = onepole(k.down[1], right[i], kDecimate);
        if (k.odd) {
            k.sub_gain = glide(k.sub_gain, sub_target);
            k.up_gain = glide(k.up_gain, up_target);
            if ((k.t & (kModTicks - 1)) == 0) modulate(k);
            tick(k, lines, l, r, listen_up, listen_sub);
            for (i32 ch = 0; ch < 2; ++ch) {
                k.held[ch][0] = k.held[ch][1];
                k.held[ch][1] = k.wet[ch];
            }
        }
        // Back to full rate: halfway between the last two, then the latest,
        // smoothed by a one-pole at 8 kHz.
        i32 o[2];
        for (i32 ch = 0; ch < 2; ++ch) {
            const i32 v = k.odd ? k.held[ch][1] : add(k.held[ch][0] >> 1, k.held[ch][1] >> 1);
            o[ch] = onepole(k.smooth[ch], v, kDecimate);
        }
        k.odd = !k.odd;
        // Width: keep the middle, scale the sides, then the autopan's sway.
        const i32 mid = add(o[0] >> 1, o[1] >> 1), side = multiply(add(o[0] >> 1, -(o[1] >> 1)), k.width);
        left[i] = multiply(add(mid, side), k.pan_left);
        right[i] = multiply(add(mid, -side), k.pan_right);
    }
    stored = k;
}
}
