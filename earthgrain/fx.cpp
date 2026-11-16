#include "core.h"
#include "native/grain.hpp"
#include "native/space.hpp"
#include <opxy/q31.hpp>

// Earthgrain, a granular looper: a send effect that records what it hears into a
// recording about 5.5 s long and plays it back as grains. When the input goes quiet it
// holds the last phrase (auto-freeze) and the grains keep reading it, so it can drone on
// its own; playing into it again lets the phrase go.
//
//   in ─► (+) ─► recording ─► grains (up to 16) ─► out
//          ▲                                  │
//          └──── Repeats ◄── tone ◄───────────┘
//
// Modes so far: Cloud (grains at random places in the last moments of the recording, or
// anywhere in a held phrase) and Stretch (grains laid evenly along a play point that moves
// slower than time, so the sound slows without changing pitch). Space (a light diffuser) is still to come.
// A send effect: it returns only the wet.
namespace earthgrain {
namespace {
using opxy::i16;
using opxy::i32;
using opxy::u32;
using opxy::q31::add;
using opxy::q31::multiply;

const i32 kMaximum = opxy::q31::maximum;
// The OP-XY's send buses run far below full scale: playing steadily peaks around 2^18
// (measured; the aux bus is 2^11 hotter). The input is scaled up by 2^10 on the way in,
// saturating, and the output back down on the way out, so the levels below (kLoud) mean what
// they say; the recording keeps its precision at any level (encode()).
const u32 kBusShift = 10;
const u32 kRate = 44100;

// The controls, in definition.json order; Mode is the Shift row's first word.
enum { kActivity, kSize, kShape, kPitch, kDrift, kRepeats, kDroop, kSpace, kMode = 8, kMotion, kSync, kHold };
// Motion's six choices: breathe, expand and contract, each bending down then up.
const u32 kMotions = 6;
// Hold's nine choices: forever, then fading over 64 s, 32 s .. 1 s, then off.
const u32 kHolds = 9;
const i32 kFull = 8388607;  // the held drone at full level, Q23
const u32 kModes = 4;  // Cloud, Stretch, Spring, Slice

// A grain: a window of the recording it reads (the whole recording, or a held phrase),
// where in the window it is (Q14 samples), its speed (Q14, negative reads backwards), its
// envelope (attack, then level, then release, each a smoothstep) and its two pan gains.
struct Grain {
    u32 on;
    u32 base;       // the window's first sample
    u32 window;     // the window's length, Q14
    u32 position;   // Q14, within the window
    i32 rate;       // Q14
    u32 age, length;
    u32 attack, release_start;
    u32 attack_scale, release_scale;  // Q31 per sample
    i32 gain_left, gain_right;        // Q31
};
const u32 kGrains = 16;

// The buffer: the grains, the last block's output (what Repeats feeds back), room kept for
// Space, then the recording (16-bit float samples, encode(), with one copy of its first sample after its end
// so a read never wraps between two samples).
const u32 kGrainWords = kGrains * sizeof(Grain) / 4;
const u32 kFeedbackOffset = kGrainWords;
// Slice's onsets: the write count of each of the last kOnsets notes or hits found.
const u32 kOnsets = 64;
const u32 kOnsetGap = kRate * 6 / 100;  // 60 ms
const i32 kOnsetFloor = 12;             // Q15
const u32 kOnsetOffset = kFeedbackOffset + opxy::block_frames;
// Space's scratch for a block (at half rate): each side diffused, half the loop's sum, and
// what it gives each side.
const u32 kHalfBlock = opxy::block_frames / 2;
const u32 kScratchOffset = kOnsetOffset + kOnsets;
const u32 kSpaceOffset = kScratchOffset + 5 * kHalfBlock;  // (scratch: the dry input at half rate, then a block for play())
const u32 kRecordingOffset = kSpaceOffset + kSpaceWords;
const u32 kLength = (kBufferWords - kRecordingOffset) * 2 - 2;  // about 5.74 s
static_assert(static_cast<unsigned long long>(kLength) * 16384 < 4294967296ull, "positions are Q14 in 32 bits");

// The input's level that counts as playing (about -50 dB, some 30 dB under steady playing on
// the bus, so a quiet send still counts), in Q15.
const i32 kLoud = 99;
const u32 kHoldAfter = kRate;           // quiet for 1 s: hold the phrase
const u32 kTail = kRate * 12 / 100;     // keep 0.12 s after the last loud moment
const u32 kPreroll = kRate / 50;        // and 20 ms before the first
const u32 kShortest = kRate / 5;        // phrases shorter than 0.2 s are not held
// A held phrase leaves 1.25 s of the recording free: room for grains still playing when it is
// held to finish, and for new recording when it is let go, without either touching it.
const u32 kLongestHeld = kLength - kRate * 5 / 4;
// Live, grains start as close behind what is playing as is safe: two blocks (2.9 ms; the
// recording runs before the grains each block), and Cloud and Spring scatter them up to 40 ms
// further back (more with Drift), so they follow the playing closely.
const u32 kNearest = 2 * opxy::block_frames;
const u32 kReach = kRate / 25;

// Pitch: the intervals grains choose among, as Q14 speeds. Normal plays at the recording's own
// pitch, never detuned; unison is the same speed but Drift detunes it, a chorus.
const i32 kSets[8][4] = {
    {16384, 16384, 16384, 16384},   // normal
    {16384, 16384, 16384, 16384},   // unison
    {16384, 8192, 16384, 8192},     // octave down
    {16384, 32768, 16384, 32768},   // octave up
    {16384, 24576, 8192, 24576},    // fifths
    {8192, 16384, 32768, 16384},    // octaves
    {16384, -16384, 16384, -16384}, // reverse
    {16384, 32768, 49152, 65536},   // harmonics
};

// cos(pi/2 x) at x = k/16, Q15: equal-power pan.
const i32 kQuarterCosine[17] = {32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170,
                                20787, 18204, 15446, 12539, 9512, 6393, 3212, 0};

// A soft knee (Q15): unchanged up to knee, then rounding off to knee + 0.25 (8192), which it
// reaches with a level slope and never passes.
i32 soft(i32 x, i32 knee) {
    const i32 m = x < 0 ? -x : x;
    if (m <= knee) return x;
    const i32 e = m - knee;
    const i32 y = e >= 16384 ? knee + 8192 : knee + e - ((e * e) >> 15);
    return x < 0 ? -y : y;
}

// The same knee for Q23 values (knee + 0.25 is 2^21).
i32 soft23(i32 x, i32 knee) {
    const i32 m = x < 0 ? -x : x;
    if (m <= knee) return x;
    const i32 e = m - knee;
    const i32 y = e >= 4194304 ? knee + 2097152 : knee + e - multiply(e << 8, e);
    return x < 0 ? -y : y;
}

// The recording keeps each sample as a small float in 16 bits: a 12-bit signed mantissa above
// a 4-bit exponent, so quiet playing keeps the same precision as loud (about 66 dB below the
// sample itself) and reading one back is two masks and a shift. Values are Q23 at full-scale level.
const i32 kTop = 8388607;
inline i16 encode(i32 v) {
    const u32 magnitude = static_cast<u32>(v < 0 ? -v : v);
    // The exponent: how many bits the magnitude has above eleven, 0..12. (Leading redundant sign
    // bits: one instruction on the device, SIGNBITS; there is no clz in its library.)
    const i32 bits = 31 - __builtin_clrsb(static_cast<i32>(magnitude));
    u32 e = bits > 11 ? static_cast<u32>(bits - 11) : 0u;
    u32 m = (magnitude + ((1u << e) >> 1)) >> e;  // rounded
    if (m > 2047) { ++e; m >>= 1; }
    const i32 mantissa = v < 0 ? -static_cast<i32>(m) : static_cast<i32>(m);
    return static_cast<i16>((static_cast<u32>(mantissa) << 4) | e);
}
// (Back to Q29: decode(), in native/grain.hpp.)

i32 control(i32 word) { return word < 0 ? 0 : word; }
u32 choice(i32 word, u32 count) {
    const u32 index = ((static_cast<u32>(control(word)) >> 16) * count) >> 15;
    return index < count ? index : count - 1;
}

u32 next(State& s) {
    u32 x = s.random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s.random = x;
}

// 2^f for f in [0, 1), Q16 in, Q30 out (minimax cubic, error about 1e-4).
i32 exp2_fraction(i32 f) {
    const i32 c1 = 1492715813, c2 = 485760852, c3 = 167932276;
    const i32 x = f << 15;
    i32 p = add(c2, multiply(c3, x));
    p = add(c1, multiply(p, x));
    return (1 << 30) + (multiply(p, x) >> 1);
}
// 2^y for y >= 0 in Q16: Q16.
u32 power_of_two(i32 y) {
    const i32 n = y >> 16;
    return static_cast<u32>(exp2_fraction(y - n * 65536) >> 14) << n;
}

u32 square_root(u32 x) {
    u32 r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

i32 smoothstep(i32 x) {  // Q31
    const i32 quarter = multiply(multiply(x, x), 1610612736 - (x >> 1));  // x^2 (3 - 2x) / 4
    return quarter >= 0x20000000 ? kMaximum : quarter << 2;  // 1 itself doesn't fit in Q31
}

i32 envelope(const Grain& g, u32 age) {
    if (age >= g.length) return 0;
    // (Held within 0..1, so even a grain left damaged in the buffer fades within its level.)
    if (age < g.attack) return smoothstep(static_cast<i32>((age * g.attack_scale) & 0x7fffffffu));
    if (age >= g.release_start) return smoothstep(static_cast<i32>(((g.length - age) * g.release_scale) & 0x7fffffffu));
    return kMaximum;
}

void start_grain(State& s, Grain* grains, u32 base, u32 window, u32 position, i32 rate, u32 length,
           i32 shape, i32 amplitude, i32 pan) {
    Grain* g = nullptr;
    for (u32 n = 0; n < kGrains; ++n)
        if (!grains[n].on) { g = &grains[n]; break; }
    if (!g) return;
    (void)s;
    g->base = base;
    g->window = window << 14;
    g->position = position << 14;
    g->rate = rate;
    g->age = 0;
    g->length = length;
    // Shape: attack from 1% to 91% of the grain; the release takes what is left, up to 70%.
    const i32 shape2 = multiply(shape, shape);
    const u32 attack_fraction = 21474836 + static_cast<u32>(multiply(shape2, 1932735283));
    u32 release_fraction = 214748365 + static_cast<u32>(multiply(kMaximum - shape, 1288490189));
    if (release_fraction > kMaximum - attack_fraction) release_fraction = kMaximum - attack_fraction;
    u32 attack = static_cast<u32>(multiply(static_cast<i32>(attack_fraction), static_cast<i32>(length)));
    u32 release = static_cast<u32>(multiply(static_cast<i32>(release_fraction), static_cast<i32>(length)));
    // At least 8 ms in and 12 ms out, so even the sharpest pluck starts and ends without a
    // tick on bright material (a shorter fade-in ticked on the device).
    if (attack < 353) attack = 353;
    if (release < 529) release = 529;
    if (attack + release > length) { attack = length / 3; release = length - attack; }
    g->attack = attack;
    g->release_start = length - release;
    g->attack_scale = 0x7fffffffu / attack;
    g->release_scale = 0x7fffffffu / release;
    // pan: Q15, -1 (left) to 1 (right).
    const u32 x = static_cast<u32>(pan + 32768) >> 1;  // 0..32767
    const u32 k = x >> 11, f = x & 2047;
    const i32 c = kQuarterCosine[k] + (((kQuarterCosine[k + 1 > 16 ? 16 : k + 1] - kQuarterCosine[k]) * static_cast<i32>(f)) >> 11);
    const u32 m = 16 - k - (f ? 1 : 0);
    const u32 mf = f ? 2048 - f : 0;
    const i32 sn = kQuarterCosine[m] + (((kQuarterCosine[m + 1 > 16 ? 16 : m + 1] - kQuarterCosine[m]) * static_cast<i32>(mf)) >> 11);
    g->gain_left = multiply(amplitude, c << 16);
    g->gain_right = multiply(amplitude, sn << 16);
    g->on = 1;
}

// Each grain's level when `overlap` (Q16) of them sound at once: 1 / sqrt(overlap), Q31 (the
// output then doubles it, so the grains come back about as loud as the input).
i32 loudness(u32 overlap) {
    return static_cast<i32>((2147483392u / square_root(overlap < 65536 ? 65536 : overlap)) << 8);
}

// Size: grains 882 x 60^size samples long (20 ms to 1.2 s).
u32 grain_length(opxy::Parameters p) {
    const u32 size = static_cast<u32>(control(p[kSize])) >> 15;  // Q16
    const u32 length = (882u * power_of_two(static_cast<i32>(((size >> 4) * (387119u >> 4)) >> 8))) >> 16;
    return length < 256 ? 256 : length;
}

// A grain's speed (Q14), one of the Pitch set's, detuned by up to 25 cents with Drift (except
// Normal), and its pan (Q15), up to 0.2 + 0.8 x Drift either side.
void voice(State& s, opxy::Parameters p, i32& rate, i32& pan) {
    const i32 drift = control(p[kDrift]);
    const u32 set = choice(p[kPitch], 8);
    rate = kSets[set][next(s) & 3];
    const i32 jitter = static_cast<i32>(next(s)) >> 16;  // Q15, either sign
    if (set) rate += (rate * ((jitter * (multiply(drift, 31138512) >> 16)) >> 15)) >> 15;
    pan = ((static_cast<i32>(next(s)) >> 16) * (6554 + (multiply(drift, 1717986918) >> 16))) >> 15;
}

// While a phrase is held, Repeats rewrites it at a point that moves through it at normal speed,
// so either side of that point the phrase was last written a whole phrase apart. A grain that
// crossed it would hear a step, so held grains keep clear of it for their whole life: how far
// behind the point the grain is (d) may not reach either side of it. Recording runs before the
// grains each block, so the point jumps a block at a time: two blocks' margin.
bool rewriting(opxy::Parameters p) { return ((control(p[kRepeats]) >> 16) * 9830 >> 15) > 16; }
const i32 kClear = 2 * opxy::block_frames;
// The grain's first read (x0, an offset in the phrase) and how far its distance behind the
// point changes over its life (change): the range x0 may take, as distances, and whether any.
bool clearance(const State& s, i32 rate, u32 len, u32 travel, i32& low, i32& high) {
    const i32 change = static_cast<i32>(len) - (rate < 0 ? -static_cast<i32>(travel) : static_cast<i32>(travel));
    low = kClear + (change < 0 ? -change : 0);
    high = static_cast<i32>(s.region_length) - kClear - (change > 0 ? change : 0);
    return low <= high;
}
i32 behind(const State& s, u32 x0) {
    i32 d = static_cast<i32>(s.rewrite) - static_cast<i32>(x0);
    return d < 0 ? d + static_cast<i32>(s.region_length) : d;
}

// How far a grain of `length` samples reads at `speed` (Q14), without overflowing: speeds go to
// 8 (the highest harmonic, bent up an octave) and grains to 1.2 s.
u32 travel_of(u32 speed, u32 length) { return ((speed >> 4) * length) >> 10; }

// Starts a grain whose middle is at `middle` (a place in the recording): inside the held
// phrase and clear of where Repeats rewrites it, or, live, far enough behind the recording.
void place(State& s, opxy::Parameters p, Grain* grains, u32 middle, i32 rate, u32 length, i32 shape,
           i32 amplitude, i32 pan) {
    const u32 speed_q14 = static_cast<u32>(rate < 0 ? -rate : rate);
    if (s.frozen) {
        u32 len = length, travel = travel_of(speed_q14, len);
        const u32 room = s.region_length - 2;
        if (travel > room) { len = (room << 10) / (speed_q14 >> 4); travel = travel_of(speed_q14, len); }
        if (len < 256) return;
        // The grain's middle where asked, kept inside the phrase.
        const u32 start = s.region_start;
        i32 at = static_cast<i32>(middle >= start ? middle - start : middle + kLength - start);
        if (at > static_cast<i32>(kLength / 2)) at -= static_cast<i32>(kLength);  // just before the phrase
        const i32 top = static_cast<i32>(room - travel);
        i32 offset = (at - static_cast<i32>(travel / 2)) % static_cast<i32>(s.region_length);  // round the phrase
        if (offset < 0) offset += static_cast<i32>(s.region_length);
        if (offset > top) offset = top;
        u32 position = rate < 0 ? static_cast<u32>(offset) + travel : static_cast<u32>(offset);
        i32 low = 0, high = 0;
        if (rewriting(p)) {
            // Too near the rewrite point: move the grain just clear of it, if it then fits.
            if (!clearance(s, rate, len, travel, low, high)) return;
            const i32 d = behind(s, position);
            if (d < low || d > high) {
                const i32 to = d < low && low - d <= d - high + static_cast<i32>(s.region_length) ? low : high;
                i32 x = static_cast<i32>(s.rewrite) - to;
                if (x < 0) x += static_cast<i32>(s.region_length);
                const i32 first = rate < 0 ? x - static_cast<i32>(travel) : x;
                if (first < 0 || first > top) return;
                position = static_cast<u32>(x);
            }
        }
        start_grain(s, grains, s.region_start, s.region_length, position, rate, len, shape, amplitude, pan);
    } else {
        const u32 travel = travel_of(speed_q14, length);
        // A backwards grain starts at its far end, so all of it must be behind the recording.
        const u32 nearest = (rate < 0 ? travel : rate > 16384 ? travel - length : 0) + kNearest;
        if (s.filled <= travel + nearest + 1) return;
        const u32 furthest = s.filled - travel - 1;
        const u32 write = s.write;
        i32 distance = static_cast<i32>(write >= middle ? write - middle : write + kLength - middle);
        if (distance > static_cast<i32>(kLength / 2)) distance -= static_cast<i32>(kLength);  // ahead of it
        distance += static_cast<i32>(travel / 2);  // distance back to where the grain starts
        if (distance < static_cast<i32>(nearest)) distance = static_cast<i32>(nearest);
        if (distance > static_cast<i32>(furthest)) distance = static_cast<i32>(furthest);
        const u32 d = static_cast<u32>(distance);
        const u32 forward = write >= d ? write - d : write + kLength - d;
        const u32 position = rate < 0 ? (forward + travel >= kLength ? forward + travel - kLength : forward + travel) : forward;
        start_grain(s, grains, 0, kLength, position, rate, length, shape, amplitude, pan);
    }
}

// How far out a cycle at `phase` (Q16) is, 0 to 1 (Q31), eased at both ends. Motion (Shift
// row): breathe out and back in, only expand (then snap back), or only contract (then snap out).
const u32 kSpringExpand = 32768;  // the share of the cycle spent expanding, Q16
i32 motion_out(opxy::Parameters p, u32 phase) {
    const u32 motion = choice(p[kMotion], kMotions) >> 1;
    const u32 out = motion == 1 ? phase >> 1
                  : motion == 2 ? (65535 - phase) >> 1
                  : phase < kSpringExpand ? (phase << 15) / kSpringExpand
                                          : ((65536 - phase) << 15) / (65536 - kSpringExpand);  // Q15
    return smoothstep(static_cast<i32>(out > 32767 ? 32767 : out) << 16);
}
// Droop: at `out` (Q31) a grain's speed bends by up to an octave, all the way out; Motion's
// down or up sets which way. At Droop 0 the bend is exactly 1.
i32 droop(opxy::Parameters p, i32 out, i32 rate) {
    const u32 depth = static_cast<u32>(control(p[kDroop])) >> 16;  // Q15 octaves
    const u32 bend = power_of_two(static_cast<i32>((depth * (static_cast<u32>(out) >> 16)) >> 14));  // Q16
    return choice(p[kMotion], kMotions) & 1 ? (rate * static_cast<i32>(bend >> 4)) >> 12 : (rate << 14) / static_cast<i32>(bend >> 2);
}
// On tempo (Sync), with a tempo to follow.
bool synced(opxy::Parameters p, const opxy::Transport& t) { return choice(p[kSync], 2) == 1 && t.bpm_x10 >= 100; }
u32 half_beat(const opxy::Transport& t) { return 13230000u / t.bpm_x10; }  // samples
// On tempo, Activity's spacings: a beat divided by 2 (1/8), 3 (1/8 triplet), 4, 6, 8, 12, 16 (1/64).
const u32 kNotes[7] = {2, 3, 4, 6, 8, 12, 16};

// Cloud and Stretch: Droop follows a slow sway, 4 s free or 2 bars on tempo (following the
// song while it plays), shaped by Motion. Returns how far out it is (Q31).
const u32 kSwaySeconds = 4, kSwayBeats = 8;
i32 sway(State& s, const opxy::Transport& t, opxy::Parameters p) {
    if (synced(p, t) && t.playing) {
        const u32 period = 96u * kSwayBeats;  // ticks
        s.sway = (t.ticks96 % period) * (0xffffffffu / period);
    } else {
        const u32 cycle = synced(p, t) ? half_beat(t) * 2 * kSwayBeats : kRate * kSwaySeconds;
        s.sway += (0xffffffffu / cycle) * opxy::block_frames;
    }
    return motion_out(p, s.sway >> 16);
}
// On tempo: whether a grain is due on the grid of notes a beat / `per_beat` long. Playing, on
// each new note of the song position; stopped, every note's length.
bool on_grid(State& s, const opxy::Transport& t, u32 per_beat) {
    const u32 ticks = 96 / per_beat;
    if (t.playing) {
        const u32 note = t.ticks96 / ticks;
        if (note == s.grid_note) return false;
        s.grid_note = note;
        s.grid_wait = 0;
        return true;
    }
    s.grid_wait -= static_cast<i32>(opxy::block_frames);
    if (s.grid_wait > 0) return false;
    s.grid_wait += static_cast<i32>(2 * half_beat(t) / per_beat);
    if (s.grid_wait <= 0) s.grid_wait = static_cast<i32>(2 * half_beat(t) / per_beat);
    return true;
}

// Cloud: grains at random, Activity² x 60 a second, each Size long (20 ms to 1.2 s).
void cloud(State& s, Grain* grains, const opxy::Transport& t, opxy::Parameters p) {
    const i32 activity = control(p[kActivity]);
    const i32 activity2 = multiply(activity, activity);
    const i32 out = sway(s, t, p);
    const u32 length = grain_length(p);
    u32 due;
    i32 amplitude;
    if (synced(p, t)) {
        // On tempo: one grain on each note of the grid Activity picks (1/8 to 1/64; none at 0).
        const u32 per_beat = kNotes[choice(activity, 7)];
        if (activity < 21474836 || !on_grid(s, t, per_beat)) return;
        due = 1;
        amplitude = loudness(((length << 8) / (2 * half_beat(t) / per_beat)) << 8);
    } else {
        s.spawn += ((static_cast<u32>(activity2) >> 16) * 5707u) >> 15;  // grains per block, Q16
        if (s.spawn < 65536) return;
        due = s.spawn >> 16;
        s.spawn &= 65535;
        const u32 per_block = ((static_cast<u32>(activity2) >> 16) * 5707u) >> 15;
        amplitude = loudness(per_block * length / opxy::block_frames);
    }

    const i32 drift = control(p[kDrift]);
    const i32 shape = control(p[kShape]);
    const u32 reach = kReach + static_cast<u32>(multiply(drift, 105840));  // 40 ms + 2.4 s x drift
    for (u32 n = 0; n < due; ++n) {
        i32 rate, pan;
        voice(s, p, rate, pan);
        rate = droop(p, out, rate);
        const u32 speed = static_cast<u32>(rate < 0 ? -rate : rate);  // Q14
        u32 travel = travel_of(speed, length);
        u32 middle;
        if (s.frozen) {
            // Anywhere in the held phrase that the whole grain fits.
            const u32 room = s.region_length - 2;
            if (travel > room) travel = room;
            middle = s.region_start + travel / 2 + next(s) % (room - travel + 1);
        } else {
            // Up to `reach` further back than the nearest a grain may start (place() keeps it
            // within what has been recorded).
            const u32 nearest = (rate < 0 ? travel : rate > 16384 ? travel - length : 0) + kNearest;
            // (the grain starts there: its middle is half its travel nearer)
            i32 at = static_cast<i32>(s.write + travel / 2) - static_cast<i32>(nearest + next(s) % reach);
            middle = static_cast<u32>(at < 0 ? at + static_cast<i32>(kLength) : at);
        }
        if (middle >= kLength) middle -= kLength;
        place(s, p, grains, middle, rate, length, shape, amplitude, pan);
    }
}

// Stretch: a play point moves through the recording at Activity² x normal speed (0 stands
// still), and grains Size long start every quarter of a grain, centred on it. Live, each new note
// played moves it to that note; between notes it falls behind, up to 3 s, then jumps to just
// behind the playing; holding, it loops through the phrase.
const u32 kStretchLag = kRate * 3;
void stretch(State& s, Grain* grains, const opxy::Transport& t, opxy::Parameters p) {
    const i32 activity = control(p[kActivity]);
    const i32 out = sway(s, t, p);
    const u32 speed = static_cast<u32>(multiply(activity, activity)) >> 15;  // Q16
    const u32 length = grain_length(p);

    // Move the play point (Q8), keeping it inside the phrase or close enough behind the recording.
    const u32 end = kLength << 8;
    s.head += (speed * opxy::block_frames) >> 8;
    if (s.head >= end) s.head -= end;
    if (s.frozen) {
        const u32 start = s.region_start << 8;
        u32 offset = s.head >= start ? s.head - start : s.head + end - start;
        if (offset >= s.region_length << 8) offset %= s.region_length << 8;
        s.head = start + offset;
        if (s.head >= end) s.head -= end;
    } else {
        const u32 write = s.write << 8;
        // A new note: the play point jumps to it (just before it starts), so what is played is
        // heard at once and stretched from there, rather than after the play point catches up.
        if (s.onsets != s.stretch_onset) {
            s.stretch_onset = s.onsets;
            const u32 back = (s.written - s.last_onset + kPreroll) << 8;
            if (back < (kStretchLag << 8)) s.head = write >= back ? write - back : write + end - back;
        }
        const u32 lag = (write >= s.head ? write - s.head : write + end - s.head) >> 8;
        const u32 most = s.filled < kStretchLag ? s.filled : kStretchLag;
        const u32 least = kNearest + length / 2;  // (place() keeps each grain behind the playing)
        if (lag < least || lag > most) {
            const u32 back = least << 8;
            s.head = write >= back ? write - back : write + end - back;
        }
    }

    // One grain every quarter of a grain: 4 overlap, so each is at half level. On tempo, one on
    // each note of the longest grid no longer than a quarter grain (1/8 to 1/64).
    u32 due;
    i32 amplitude;
    if (synced(p, t)) {
        u32 k = 0;
        while (k < 6 && 2 * half_beat(t) / kNotes[k] > length / 4) ++k;
        if (!on_grid(s, t, kNotes[k])) return;
        due = 1;
        amplitude = loudness(((length << 8) / (2 * half_beat(t) / kNotes[k])) << 8);
    } else {
        s.spawn += (opxy::block_frames << 18) / length;
        if (s.spawn < 65536) return;
        due = s.spawn >> 16;
        s.spawn &= 65535;
        amplitude = 1 << 30;
    }

    const i32 drift = control(p[kDrift]);
    const i32 shape = control(p[kShape]);
    // Scatter around the play point: a quarter grain always (so the grains don't comb), up to
    // 0.5 s more with Drift.
    const u32 scatter = length / 4 + static_cast<u32>(multiply(drift, 22050)) + 1;
    for (u32 n = 0; n < due; ++n) {
        i32 rate, pan;
        voice(s, p, rate, pan);
        rate = droop(p, out, rate);
        const i32 shift = static_cast<i32>(next(s) % (2 * scatter)) - static_cast<i32>(scatter);
        const u32 head = s.head >> 8;
        const i32 middle = static_cast<i32>(head) + shift;
        place(s, p, grains, middle < 0 ? static_cast<u32>(middle + static_cast<i32>(kLength))
                            : static_cast<u32>(middle) >= kLength ? static_cast<u32>(middle) - kLength : static_cast<u32>(middle),
              rate, length, shape, amplitude, pan);
    }
}

// Spring: grains whose spacing breathes, like a spring expanding and contracting. Compressed,
// the grains come close together (Activity: 64 ms apart to 8 ms); expanded, a quarter of the
// cycle apart, up to 0.6 s. Shape is the spring's size: a whole cycle, out and back, takes
// 0.25 s to 30 s, a small spring opening only a little. Drift jitters the gaps and moves each
// cycle further from the last.
const bool kSpringMove = false;   // the place it plays moves on at half speed through a cycle
const i32 kSpringShape = 858993459;  // its grains' own envelope: a rounded pluck (0.4)
void spring(State& s, Grain* grains, const opxy::Transport& t, opxy::Parameters p) {
    // The cycle: free, 0.25 s x 120^shape, up to 30 s; with Sync on tempo, half a beat to 64
    // beats, following the song position while it plays.
    const i32 shape = control(p[kShape]);
    const bool synced = earthgrain::synced(p, t);
    const u32 half_beat = synced ? earthgrain::half_beat(t) : 0;  // samples
    const u32 length_octaves = (static_cast<u32>(shape) >> 15) * 6907u / 1000u;  // Q16: 120^shape = 2^(6.907 shape)
    const u32 halves = 1u << choice(shape, 8);                   // the cycle in half beats: 1 to 128
    const u32 cycle = synced ? half_beat * halves
                             : (11025u * (power_of_two(static_cast<i32>(length_octaves)) >> 8)) >> 8;
    const u32 before = s.spring_phase;
    if (synced && t.playing && t.ticks96 != s.spring_tick) {
        const u32 period = 48u * halves;  // ticks
        s.spring_phase = (t.ticks96 % period) * (0xffffffffu / period);
        s.spring_tick = t.ticks96;
    } else {
        s.spring_phase += (0xffffffffu / cycle) * opxy::block_frames;
    }
    s.spring_elapsed += opxy::block_frames;
    // A new cycle, compressed again (a small step back, following the song, is not one).
    const bool begins = s.spring_phase < before && before - s.spring_phase > 0x80000000u;
    if (begins && synced) s.spring_wait = 0;  // a grain on the beat
    const i32 drift = control(p[kDrift]);
    const u32 length = grain_length(p);
    if (begins || !s.spring_anchor) {
        // Where it plays: live, just behind the playing; holding, a step on from the last.
        s.spring_elapsed = 0;
        if (s.frozen) {
            // Holding, the anchor is how far into the phrase it plays: hold() starts it just
            // after the lead-in; each cycle steps on by up to the whole phrase with Drift.
            const u32 step = 1 + static_cast<u32>(multiply(drift, static_cast<i32>(s.region_length)));
            s.spring_anchor += next(s) % step;
            while (s.spring_anchor >= s.region_length) s.spring_anchor -= s.region_length;
            if (!s.spring_anchor) s.spring_anchor = 1;
        } else {
            // Live, how far behind the playing it repeats, so it follows what is played.
            const u32 back = next(s) % (kReach + static_cast<u32>(multiply(drift, 105840)));
            if (synced) {
                // On tempo, a whole number of note values back (at least one), so a grain on
                // the beat repeats a moment that was on the beat too.
                const u32 note = 2 * half_beat / kNotes[choice(control(p[kActivity]), 7)];
                s.spring_anchor = (back / note + 1) * note;
            } else {
                s.spring_anchor = kNearest + back;
            }
        }
    }

    // How far out it is (0 compressed, 1 expanded), Q31, shaped by Motion.
    const i32 stretch = motion_out(p, s.spring_phase >> 16);
    // The gap: fastest x 2^(octaves x stretch). Shape is the spring's size: a small one is quick
    // and opens a little, a large one slow and wide. Its widest gap is a quarter of its cycle,
    // up to 0.6 s free (16 note values on tempo), and never closer than the fastest. Free,
    // fastest is 353 x 2^(3 (1 - activity)) samples; on tempo, a note value (1/8 to 1/64).
    // (In octaves, Q16: 353 samples is 8.464, 0.6 s 14.69, a quarter of 0.25 s 11.428.)
    const i32 activity = control(p[kActivity]);
    const u32 slack = (static_cast<u32>(kMaximum - activity) >> 15) * 3;  // Q16 octaves
    u32 fastest = (353u * power_of_two(static_cast<i32>(slack))) >> 16;
    const i32 widest = static_cast<i32>(748970u + length_octaves);  // a quarter cycle
    i32 open = (widest < 962722 ? widest : 962722) - static_cast<i32>(554701u + slack);
    if (synced) {
        // A quarter of the cycle in note values: halves x notes-a-beat / 8.
        static const i32 kNoteOctaves[7] = {65536, 103872, 131072, 169408, 196608, 234944, 262144};  // log2 kNotes
        const u32 k = choice(activity, 7);
        fastest = 2 * half_beat / kNotes[k];
        open = static_cast<i32>(choice(shape, 8) << 16) + kNoteOctaves[k] - (3 << 16);
        if (open > (4 << 16)) open = 4 << 16;
    }
    const u32 octaves = open > 0 ? static_cast<u32>(open) : 0u;
    const u32 widen = power_of_two(static_cast<i32>(((octaves >> 2) * (static_cast<u32>(stretch) >> 17)) >> 12));
    u32 gap = (fastest * (widen >> 6)) >> 10;
    if (synced) gap = ((gap + fastest / 2) / fastest) * fastest;  // on tempo, whole note values: every grain on the grid
    // Never wait longer than the gap is now (with Drift's jitter): when Shape or Activity
    // shortens it, the next grain comes sooner rather than after a silence.
    s.spring_wait -= static_cast<i32>(opxy::block_frames);
    const i32 longest = static_cast<i32>(gap + (gap >> 2));
    if (s.spring_wait > longest) s.spring_wait = longest;
    if (s.spring_wait > 0) return;

    i32 rate, pan;
    voice(s, p, rate, pan);
    // Droop: as it expands the pitch bends, and comes back as it contracts.
    rate = droop(p, stretch, rate);
    // Live, every grain repeats the same moment the same distance behind the playing, so
    // overlapping grains add up in phase: each is 1 / overlap (at most the pool), not
    // 1 / sqrt(overlap) as for grains from different moments (held, they start apart in time).
    u32 overlap = ((length << 8) / gap) << 8;  // Q16
    if (overlap < 65536) overlap = 65536;
    if (overlap > (kGrains << 16)) overlap = kGrains << 16;
    const i32 amplitude = s.frozen ? loudness(overlap) : static_cast<i32>((0x7fffffffu / (overlap >> 8)) << 8);
    // The grain begins at the anchor: in the phrase, or that far behind the playing.
    u32 middle;
    if (s.frozen) {
        middle = s.region_start + s.spring_anchor + length / 2;
        while (middle >= kLength) middle -= kLength;
    } else {
        const i32 at = static_cast<i32>(s.write + length / 2) - static_cast<i32>(s.spring_anchor);
        middle = static_cast<u32>(at < 0 ? at + static_cast<i32>(kLength) : at);
    }
    if (kSpringMove) {
        middle += s.spring_elapsed / 2;
        if (middle >= kLength) middle -= kLength;
    }
    place(s, p, grains, middle, rate, length, kSpringShape, amplitude, pan);

    // The gap to the next, a little jittered by Drift (up to 25%).
    const i32 wobble = synced ? 0 : ((static_cast<i32>(next(s)) >> 16) * (multiply(drift, 8192))) >> 15;
    s.spring_wait += static_cast<i32>(gap) + ((static_cast<i32>(gap) * wobble) >> 15);
    if (s.spring_wait <= 0) s.spring_wait = static_cast<i32>(gap);
}

// Slice: the phrase cut where its notes or hits begin, the slices played one after another,
// in order at Drift 0 and more and more at random as it rises. Activity: how often a slice
// plays (0.6 s apart to 37 ms; on tempo, one on each note of a 1/8 to 1/64 grid); Size: at
// most how much of each slice plays. Live it slices the last 4 s; held, the phrase. With no
// onsets to cut at (a pad, say), the phrase is cut into eight even slices.

const u32 kSliceLive = kRate * 4;
const u32 kSlicePreroll = 353;          // 8 ms early, so a slice's fade-in is done by its onset
void slice(State& s, Grain* grains, const u32* onsets, const opxy::Transport& t, opxy::Parameters p) {
    const i32 activity = control(p[kActivity]);
    const i32 out = sway(s, t, p);
    if (activity < 21474836) return;  // under 1%: none
    u32 gap;
    if (synced(p, t)) {
        const u32 per_beat = kNotes[choice(activity, 7)];
        if (!on_grid(s, t, per_beat)) return;
        gap = 2 * half_beat(t) / per_beat;
    } else {
        gap = (26460u << 10) / (power_of_two(static_cast<i32>((static_cast<u32>(activity) >> 15) * 4)) >> 6);
        s.slice_wait -= static_cast<i32>(opxy::block_frames);
        if (s.slice_wait > static_cast<i32>(gap)) s.slice_wait = static_cast<i32>(gap);
        if (s.slice_wait > 0) return;
        s.slice_wait += static_cast<i32>(gap);
        if (s.slice_wait <= 0) s.slice_wait = static_cast<i32>(gap);
    }

    // The range slices come from: the held phrase, or the last 4 s (as much as is recorded).
    u32 start, span;
    if (s.frozen) {
        start = s.region_start;
        span = s.region_length;
    } else {
        span = s.filled < kSliceLive ? s.filled : kSliceLive;
        start = s.write >= span ? s.write - span : s.write + kLength - span;
    }
    if (span < kRate / 5) return;
    // Where each onset is in the range (onsets are in the order played, so these rise).
    const u32 newest = s.onsets, oldest = newest > kOnsets ? newest - kOnsets : 0;
    u32 count = 0;
    for (u32 n = oldest; n < newest; ++n) {
        const u32 age = s.written - onsets[n % kOnsets];
        if (age == 0 || age >= kLength) continue;
        const u32 at = s.write >= age ? s.write - age : s.write + kLength - age;
        const u32 offset = at >= start ? at - start : at + kLength - start;
        if (offset < span) ++count;
    }
    const bool even = count < 2;
    const u32 slices = even ? 8 : count;
    // The next slice: the one after the last, or with Drift's chance, any.
    u32 k = s.slice + 1;
    if ((next(s) >> 17) < (static_cast<u32>(control(p[kDrift])) >> 16)) k = next(s) % slices;
    if (k >= slices) k = 0;
    s.slice = k;
    u32 from, to;
    if (even) {
        from = span / 8 * k;
        to = from + span / 8;
    } else {
        from = span, to = span;
        u32 m = 0;
        for (u32 n = oldest; n < newest; ++n) {
            const u32 age = s.written - onsets[n % kOnsets];
            if (age == 0 || age >= kLength) continue;
            const u32 at = s.write >= age ? s.write - age : s.write + kLength - age;
            const u32 offset = at >= start ? at - start : at + kLength - start;
            if (offset >= span) continue;
            if (m == k) from = offset;
            else if (m == k + 1) { to = offset; break; }
            ++m;
        }
    }
    const u32 most = grain_length(p);
    u32 length = to - from + kSlicePreroll;
    if (length > most) length = most;
    if (length < 256) length = 256;
    i32 rate, pan;
    voice(s, p, rate, pan);
    rate = droop(p, out, rate);
    const u32 speed = static_cast<u32>(rate < 0 ? -rate : rate);
    const u32 first = from > kSlicePreroll ? from - kSlicePreroll : 0;
    u32 middle = start + first + travel_of(speed, length) / 2;
    if (middle >= kLength) middle -= kLength;
    const i32 amplitude = loudness(((length << 8) / gap) << 8);
    place(s, p, grains, middle, rate, length, control(p[kShape]), amplitude, pan);
}

// Mixes one grain's next block into the output (Q26, so 16 grains at full scale fit). `v` is
// a block of scratch for grain() (native/grain.hpp), which does the unchecked stretches.
void play(Grain& g, const i16* recording, i32* out_left, i32* out_right, i32* v) {
    u32 count = g.length - g.age;
    if (count > opxy::block_frames) count = opxy::block_frames;
    u32 position = g.position;
    const u32 window = g.window, base = g.base;
    const i32 rate = g.rate;
    // Most blocks reach neither the end of the window nor the end of the recording: then the
    // loop needs no checks. The rest take the careful loop.
    const u32 last = position + static_cast<u32>(rate) * (count - 1);  // where the block ends (either way)
    const u32 low = rate < 0 ? last : position, high = rate < 0 ? position : last;
    const bool clear = low <= high && high < window && base + (high >> 14) + 1 < kLength;
    const i16* r = recording + base;
    // At full level one gain does for the block. In the attack and release the envelope is
    // followed every 16 samples, so even a short pluck's curve stays smooth.
    const u32 segment = g.age >= g.attack && g.age + count <= g.release_start ? count : 16;
    const i32 e0 = envelope(g, g.age);
    i32 left = multiply(e0, g.gain_left), right = multiply(e0, g.gain_right);
    for (u32 done = 0; done < count;) {
        const u32 n = count - done < segment ? count - done : segment;
        const i32 e = envelope(g, g.age + done + n);  // 0 at the grain's end
        const i32 to_left = multiply(e, g.gain_left), to_right = multiply(e, g.gain_right);
        i32 step_left, step_right;
        if (n == 16) { step_left = (to_left >> 4) - (left >> 4); step_right = (to_right >> 4) - (right >> 4); }
        else if (to_left == left && to_right == right) step_left = step_right = 0;  // (at full level: no division)
        else { step_left = (to_left - left) / static_cast<i32>(n); step_right = (to_right - right) / static_cast<i32>(n); }
        i32* ol = out_left + done;
        i32* orr = out_right + done;
        if (clear) {
            GrainJob job{position, rate, n, left, step_left, right, step_right, orr, v};
            grain(r, ol, &job);
            position = job.position;
        } else {
            for (u32 i = 0; i < n; ++i) {
                u32 index = base + (position >> 14);
                if (index >= kLength) index -= kLength;
                const i32 a = decode(recording[index]), b = decode(recording[index + 1]);
                const i32 v = (a + multiply(b - a, static_cast<i32>(position << 18 >> 1))) << 2;  // (the fraction, Q31)
                ol[i] += multiply(v, left) >> 5;
                orr[i] += multiply(v, right) >> 5;
                left += step_left;
                right += step_right;
                position += static_cast<u32>(rate);
                if (position >= window) position += rate < 0 ? window : 0u - window;
            }
        }
        left = to_left;
        right = to_right;
        done += n;
    }
    g.position = position;
    g.age += count;
    if (g.age >= g.length) g.on = 0;
}

void hold(State& s) {
    // The phrase: what was recorded since it began, less all but 0.12 s of the quiet after.
    const u32 quiet = s.clock - s.last_loud;
    const u32 trim = quiet > kTail ? quiet - kTail : 0;
    u32 length = s.recorded > trim ? s.recorded - trim : 0;
    if (length > s.filled) length = s.filled;
    if (length > kLongestHeld - trim) length = kLongestHeld - trim;  // the room counts from the recording
    s.recorded = 0;
    if (length < kShortest) return;
    const u32 end = s.write >= trim ? s.write - trim : s.write + kLength - trim;
    s.region_start = end >= length ? end - length : end + kLength - length;
    s.region_length = length;
    s.rewrite = 0;
    s.frozen = 1;
    s.draining = 1;
    s.spring_anchor = kPreroll;  // Spring repeats the phrase's start first
    // Stretch carries on from where it was playing if that is in the phrase; otherwise it
    // starts just after the lead-in kept before the first loud moment.
    const u32 head = s.head >> 8;
    const u32 into = head >= s.region_start ? head - s.region_start : head + kLength - s.region_start;
    if (into >= length) {
        const u32 from = s.region_start + kPreroll;
        s.head = (from >= kLength ? from - kLength : from) << 8;
    }
}
}  // namespace

void initialize(State& state) {
    state.ready = 0;
    state.random = 0x9e3779b9u;
    state.clock = 0;
    state.write = 0;
    state.filled = 0;
    state.recorded = 0;
    state.last_loud = 0;
    state.frozen = 0;
    state.draining = 0;
    state.region_start = state.region_length = state.rewrite = 0;
    state.spawn = 0;
    state.head = 0;
    state.spring_wait = 0;
    state.spring_phase = state.spring_anchor = state.spring_elapsed = state.spring_tick = 0;
    state.level = 0;
    state.tone = state.dc = state.loudest = state.tone_level = state.held_level = 0;
    state.lift = 4096;
    state.trim = 32767;
    state.fade = kFull;
    state.sway = 0;
    state.grid_wait = 0;
    state.grid_note = 0xffffffffu;
    state.written = state.onsets = state.last_onset = state.stretch_onset = 0;
    state.onset_level = 0;
    state.slice_wait = 0;
    state.slice = 0;
    state.space_at = 0;
    for (u32 k = 0; k < 4; ++k) state.space_damp[k] = 0;
    state.space_cleared = 0;
    state.space_last[0] = state.space_last[1] = 0;
    state.quiet_blocks = state.asleep = 0;
    state.rest[0] = state.rest[1] = 0;
}

// Space: a light reverb after the grains (not fed back by Repeats). Each side passes through
// two diffusers, then into a loop of four lines mixed together each time round (a Householder
// mix), darkened a little as they go; Space sets how much of it is heard and how long it
// lasts, about 0.4 s to 2.3 s. It runs at half rate, its eight lines sharing 4096 words of the
// buffer (native/space.hpp). Cleared a block at a time at first.
// (In samples at half rate: Space runs at 22.05 kHz, its loop darkening the top anyway.)
static_assert(2406 + 937 + 1 <= kSpaceWords, "Space's lines fit (native/space.hpp's last line)");
const i32 kSpaceWet = 1503238554;   // 0.7 (Q31)
void space(State& s, i32* words, i32* scratch, i32* left, i32* right, opxy::Parameters p) {
    if (s.space_cleared < kSpaceWords) {
        const u32 end = s.space_cleared + 1024 < kSpaceWords ? s.space_cleared + 1024 : kSpaceWords;
        for (u32 n = s.space_cleared; n < end; ++n) words[n] = 0;
        s.space_cleared = end;
        return;
    }
    const i32 amount = control(p[kSpace]);
    if (amount < 21474836) return;  // under 1%: none (its tail waits in the lines)
    // How long it lasts: 0.3 s x 2^(3.3 amount); each loop line's gain loses 60 dB in that time.
    const u32 lasts = (13230u * (power_of_two(multiply(amount, 216269)) >> 8)) >> 8;  // samples
    const i32 dry = kMaximum - (amount >> 1), wet = multiply(amount, kSpaceWet);
    const u32 half_lasts = lasts / 2;
    i32 gain[4];
    for (u32 k = 0; k < 4; ++k)
        gain[k] = static_cast<i32>(power_of_two(static_cast<i32>(15u * 65536u - 653131u * kSpaceLengths[4 + k] / half_lasts)));
    // The block: native/space.hpp (scratch holds the dry input process() left, at half rate).
    SpaceJob job;
    job.left = left;
    job.right = right;
    job.at = s.space_at;
    job.dry = dry;
    job.wet = wet;
    for (u32 k = 0; k < 4; ++k) {
        job.gain[k] = gain[k];
        job.damp[k] = s.space_damp[k];
    }
    job.last[0] = s.space_last[0];
    job.last[1] = s.space_last[1];
    space_block(words, scratch, &job);
    for (u32 k = 0; k < 4; ++k) s.space_damp[k] = job.damp[k];
    s.space_last[0] = job.last[0];
    s.space_last[1] = job.last[1];
    s.space_at = (s.space_at - kHalfBlock) & kSpaceMask;
}

// Idle. The send bus plays at about 2^18 (Q31) when steady, so a swing of 2^6 is about 72 dB
// under it. The effect rests once nothing more can sound (a held drone faded to nothing by
// Hold, or, not holding, a recording quiet for its whole length, so no grain has anything to
// play) and the input and the output have both been that still for a quarter second (Space's
// tail gone too). Resting costs a block's swing check; it holds its last output (Space can
// leave a steady offset, which is no sound) until the input moves again. A gap between grains
// (Spring's and Slice's can be long) is not an end, and Hold forever never ends. Stillness is
// judged by each side's swing within a block, not its level, so that offset does not keep it
// awake.
const u32 kQuiet = 1 << 6;
const u32 kSleepAfter = 172;  // blocks: a quarter second

// The larger side's swing in a block: its highest sample less its lowest.
u32 block_swing(const opxy::Channel left, const opxy::Channel right) {
    i32 lo[2] = {left[0], right[0]}, hi[2] = {left[0], right[0]};
    for (u32 i = 1; i < opxy::block_frames; ++i) {
        lo[0] = left[i] < lo[0] ? left[i] : lo[0];
        hi[0] = left[i] > hi[0] ? left[i] : hi[0];
        lo[1] = right[i] < lo[1] ? right[i] : lo[1];
        hi[1] = right[i] > hi[1] ? right[i] : hi[1];
    }
    const u32 a = static_cast<u32>(hi[0]) - static_cast<u32>(lo[0]), b = static_cast<u32>(hi[1]) - static_cast<u32>(lo[1]);
    return a > b ? a : b;
}

// The state and the grain pool can hold what an earlier build, or another effect in the slot,
// left there: anything out of range starts again rather than reading outside the recording.
bool sound(const State& s) {
    return s.ready <= 1 && s.frozen <= 1 && s.draining <= 1 && s.asleep <= 1 && s.write < kLength && s.filled <= kLength &&
           s.recorded <= kLength && s.region_start < kLength && s.region_length <= kLength &&
           s.rewrite <= s.region_length && (s.head >> 8) < kLength && s.spring_anchor < kLength &&
           s.fade >= 0 && s.fade <= kFull && s.loudest >= 0 && s.loudest <= 65535 && s.dc > -(1 << 28) && s.dc < (1 << 28) &&
           s.tone_level >= 0 && s.tone_level <= (1 << 24) && s.held_level >= 0 && s.held_level <= 65535 &&
           s.lift >= 1024 && s.lift <= 16384 && s.trim >= 16384 && s.trim <= 32767 && s.space_at <= kSpaceMask && s.space_cleared <= kSpaceWords;
}
void check_grains(Grain* grains) {
    const i32 fastest = 1 << 18;  // the highest harmonic, bent up an octave, with room to spare
    for (u32 n = 0; n < kGrains; ++n) {
        Grain& g = grains[n];
        if (!g.on) continue;
        const bool ok = g.on == 1 && g.base < kLength && g.window >= (1u << 14) && g.window <= (kLength << 14) &&
                        g.position < g.window && g.rate != 0 && g.rate <= fastest && g.rate >= -fastest &&
                        g.length <= kLength && g.age < g.length && g.attack > 0 && g.attack <= g.length &&
                        g.release_start < g.length && g.gain_left >= 0 && g.gain_right >= 0;
        if (!ok) g.on = 0;
    }
}

void process(State& state, opxy::Buffer buffer, const opxy::Transport& transport, opxy::Channel left,
             opxy::Channel right, opxy::Parameters parameters) {
    const u32 frames = opxy::block_frames;
    if (!buffer.words || buffer.count < kBufferWords) {
        for (u32 i = 0; i < frames; ++i) left[i] = right[i] = 0;
        return;
    }
    Grain* grains = reinterpret_cast<Grain*>(buffer.words);
    i32* feedback = buffer.words + kFeedbackOffset;
    i16* recording = reinterpret_cast<i16*>(buffer.words + kRecordingOffset);
    if (!sound(state)) initialize(state);
    if (!state.ready) {
        for (u32 n = 0; n < kGrains; ++n) grains[n].on = 0;
        for (u32 i = 0; i < frames; ++i) feedback[i] = 0;
        state.ready = 1;
    }
    const u32 heard_in = block_swing(left, right);
    if (state.asleep) {
        if (heard_in <= kQuiet) {
            for (u32 i = 0; i < frames; ++i) {
                left[i] = state.rest[0];
                right[i] = state.rest[1];
            }
            return;
        }
        state.asleep = 0;
        state.quiet_blocks = 0;
    }

    // Record. Repeats feeds the last block's output back in, darkened: while recording it
    // mixes into what is written, and while holding it slowly rewrites the held phrase.
    const i32 repeats = control(parameters[kRepeats]) >> 16;  // Q15
    const i32 feed = (repeats * 19661) >> 15;                 // up to 0.6
    const i32 rewrite = (repeats * 9830) >> 15;               // up to 0.3
    i32 peak = 0, tone = state.tone, tone_peak = 0, dc = state.dc;
    // Holding, Repeats rewrites the phrase with what the grains play, which comes from elsewhere
    // in the phrase: two unrelated sounds, so a plain crossfade would lose level every pass and
    // the drone would fade into quiet stretches. Instead what is fed back is brought to the
    // phrase's own loudness (as it was played; 1/4 to 4 times) and mixed in at equal power. The
    // two are partly alike, which would let it grow, so while the phrase is louder than it was
    // played each sample rewritten is turned down by as much (at most by half a pass).
    // Both move a sixteenth of the way each block, and smoothly sample by sample, so the
    // phrase is never rewritten with a step in it.
    i32 lift_to = state.tone_level > 0 ? state.loudest * 4096 / state.tone_level : 4096;
    lift_to = lift_to < 1024 ? 1024 : lift_to > 16384 ? 16384 : lift_to;
    i32 trim_to = state.held_level > state.loudest ? state.loudest * 32768 / state.held_level : 32767;  // Q15
    trim_to = trim_to < 16384 ? 16384 : trim_to > 32767 ? 32767 : trim_to;
    lift_to = state.lift + ((lift_to - state.lift) >> 4);
    trim_to = state.trim + ((trim_to - state.trim) >> 4);
    i32 lift = state.lift << 6, trim = state.trim << 6;  // Q18 and Q21, a 64th of the change a sample
    const i32 lift_step = lift_to - state.lift, trim_step = trim_to - state.trim;
    i32 held_peak = 0;
    const i32 keep = static_cast<i32>(square_root(static_cast<u32>((1 << 30) - rewrite * rewrite)));  // Q15
    u32* onsets = reinterpret_cast<u32*>(buffer.words + kOnsetOffset);
    // An onset: the input jumps above three times where it has lately been (and above a floor about
    // 68 dB under full scale, so quiet sends are cut too), at least 60 ms after the last.
    const i32 onset_above = 3 * state.onset_level + kOnsetFloor;
    // (The loop keeps the state it moves in locals: written through `onsets` and `recording`,
    // the state would otherwise be read back from memory every sample.)
    u32 write = state.write, written = state.written, onset_count = state.onsets, last_onset = state.last_onset;
    u32 rewrite_at = state.rewrite, draining = state.draining;
    const u32 frozen = state.frozen, region_start = state.region_start, region_length = state.region_length;
    for (u32 i = 0; i < frames; ++i) {
        const i32 sum = (left[i] >> 1) + (right[i] >> 1);
        // Q23 at full-scale level, keeping every bit the bus has (Q15 for the level).
        const i32 x = sum > (kTop >> (kBusShift - 8)) ? kTop : sum < -(kTop >> (kBusShift - 8)) ? -kTop : sum << (kBusShift - 8);
        const i32 magnitude = (x < 0 ? -x : x) >> 8;
        if (magnitude > peak) peak = magnitude;
        // Without DC (about 10 Hz and below): rounding round the loop would otherwise build an
        // offset pass after pass, until it was as loud as the phrase.
        dc += multiply(feedback[i] - dc, 3060000);  // about 0.0014
        tone += multiply(feedback[i] - dc - tone, 644218880);  // 0.3
        const i32 tone_magnitude = (tone < 0 ? -tone : tone) >> 8;
        if (tone_magnitude > tone_peak) tone_peak = tone_magnitude;
        if (draining && write == region_start) draining = 0;  // never into the phrase
        if (!frozen || draining) {
            // The fed-back part stays within half scale, so at high Repeats the loop saturates
            // gently rather than growing until it clips.
            i32 v = x + (feed ? soft23(multiply(tone, feed << 16), 2097152) : 0);
            v = v > kTop ? kTop : v < -kTop ? -kTop : v;
            const i16 c = encode(v);
            recording[write] = c;
            if (write == 0) recording[kLength] = c;
            if (magnitude > onset_above && (!onset_count || written - last_onset > kOnsetGap)) {
                onsets[onset_count % kOnsets] = written;
                ++onset_count;
                last_onset = written;
            }
            if (++write == kLength) write = 0;
            ++written;
        }
        if (frozen) {
            if (rewrite > 16) {
                u32 index = region_start + rewrite_at;
                if (index >= kLength) index -= kLength;
                const i32 was = decode(recording[index]) >> 6;
                const i32 was_magnitude = (was < 0 ? -was : was) >> 8;
                if (was_magnitude > held_peak) held_peak = was_magnitude;
                lift += lift_step;
                trim += trim_step;
                i32 toward = multiply(tone, (lift >> 6) << 15);  // tone x lift / 16 (Q23 / 16)
                toward = toward > (kTop >> 4) ? kTop : toward < -(kTop >> 4) ? -kTop : toward << 4;
                i32 v = multiply(multiply(was, keep << 16) + multiply(toward, rewrite << 16), (trim >> 6) << 16);
                v = v > kTop ? kTop : v < -kTop ? -kTop : v;
                const i16 c = encode(v);
                recording[index] = c;
                if (index == 0) recording[kLength] = c;
            }
            if (++rewrite_at >= region_length) rewrite_at = 0;
        }
    }
    state.write = write;
    state.written = written;
    state.onsets = onset_count;
    state.last_onset = last_onset;
    state.rewrite = rewrite_at;
    state.draining = draining;
    state.tone = tone;
    state.dc = dc;
    state.lift = lift_to;
    state.trim = trim_to;
    state.tone_level = tone_peak > state.tone_level ? state.tone_level + (((tone_peak - state.tone_level) * 19661) >> 15)
                                                    : (state.tone_level * 32453) >> 15;
    state.held_level = !state.frozen ? 0
                     : held_peak > state.held_level ? state.held_level + (((held_peak - state.held_level) * 19661) >> 15)
                                                    : (state.held_level * 32453) >> 15;
    // The level onsets are found against: rising over about 20 ms (so a note that swells in
    // over 80 ms still jumps above it) and falling over 50 ms.
    state.onset_level = peak > state.onset_level ? state.onset_level + (((peak - state.onset_level) * 2300) >> 15)
                                                 : state.onset_level * 31818 >> 15;
    if (!state.frozen || state.draining) {
        state.filled = state.filled + frames < kLength ? state.filled + frames : kLength;
        if (state.recorded) state.recorded = state.recorded + frames < kLength ? state.recorded + frames : kLength;
    }
    state.clock += frames;

    // Auto-freeze: playing begins a phrase; a second of quiet holds it; playing again lets it go.
    state.level = peak > state.level ? state.level + (((peak - state.level) * 19661) >> 15)
                                     : (state.level * 32453) >> 15;
    // The last loud moment from the block's own peak, not the level (which falls slowly, and
    // would leave most of a second of quiet at the end of the held phrase).
    if (peak > kLoud) state.last_loud = state.clock;
    if (!state.frozen && state.level > state.loudest) state.loudest = state.level;
    if (state.level > kLoud) {
        if (state.frozen && state.level > 2 * kLoud) state.frozen = state.recorded = 0;
        if (!state.frozen && !state.recorded) {
            // A new phrase: Stretch and Spring start again from what is playing now.
            state.recorded = kPreroll;
            state.loudest = 0;
            state.head = state.write << 8;
            state.spring_anchor = 0;
        }
    } else if (!state.frozen && state.recorded && state.clock - state.last_loud > kHoldAfter) {
        // Hold off holds nothing: the grains run out as the recording goes quiet.
        if (choice(parameters[kHold], kHolds) < kHolds - 1) hold(state);
        else state.recorded = 0;
    }
    // Hold: held, the drone holds for good (the first choice) or fades away over 64 s, 32 s ..
    // 1 s.
    const u32 holding = choice(parameters[kHold], kHolds);
    if (!state.frozen || holding == 0) {
        state.fade = kFull;
    } else if (state.fade > 0) {
        const u32 time = kRate << (holding < 7 ? 7 - holding : 0);  // samples
        state.fade -= static_cast<i32>(536870848u / time);
        if (state.fade < 0) state.fade = 0;
    }
    const i32 fade_q15 = state.fade >> 8;
    const i32 gain = (fade_q15 * fade_q15) << 1;  // Q31: squared, so the tail dies away smoothly

    // Grains begin once a tenth of a second is recorded (or a phrase is held).
    if (state.frozen || state.filled >= kRate / 10) switch (choice(parameters[kMode], kModes)) {
    case 1: stretch(state, grains, transport, parameters); break;
    case 2: spring(state, grains, transport, parameters); break;
    case 3: slice(state, grains, onsets, transport, parameters); break;
    default: cloud(state, grains, transport, parameters); break;
    }

    check_grains(grains);
    // The dry input, for Space (a send returns only its wet: the space of what is played, never
    // the playing itself again): at half rate, at the level grains of it would have (half the
    // input's, in Q26: the output doubles them), halved again for headroom as Space halves the grains.
    {
        i32* dry_left = buffer.words + kScratchOffset;
        i32* dry_right = dry_left + kHalfBlock;
        const i32 top = 1 << 27;
        for (u32 i = 0; i < kHalfBlock; ++i) {
            i32 a = (left[2 * i] >> 1) + (left[2 * i + 1] >> 1), b = (right[2 * i] >> 1) + (right[2 * i + 1] >> 1);
            a = a > top - 1 ? top - 1 : a < -top ? -top : a;
            b = b > top - 1 ? top - 1 : b < -top ? -top : b;
            dry_left[i] = a << (kBusShift - 7);  // (twice a quarter of the pair, as Space takes the grains)
            dry_right[i] = b << (kBusShift - 7);
        }
    }
    for (u32 i = 0; i < frames; ++i) left[i] = right[i] = 0;
    // Grains reading the whole recording (not a held phrase) keep it recording until they end.
    u32 live = 0;
    for (u32 n = 0; n < kGrains; ++n)
        if (grains[n].on) {
            play(grains[n], recording, left, right, buffer.words + kScratchOffset + 2 * kHalfBlock);
            if (grains[n].on && grains[n].window == kLength << 14) live = 1;
        }
    if (!live || !state.frozen) state.draining = 0;
    for (u32 i = 0; i < frames; ++i) feedback[i] = ((left[i] >> 1) + (right[i] >> 1)) >> 3;  // Q23
    space(state, buffer.words + kSpaceOffset, buffer.words + kScratchOffset, left, right, parameters);
    // Doubled, then rounded off above three-quarters scale, and back to the bus's level. Most
    // samples are well under the knee (Q15 24575, doubled): those only shift.
    const i32 under = 24575 << 10, over = 24576 << 10;
    const auto out = [](i32 v) {
        const i32 q = v > 33554431 ? 65535 : v < -33554432 ? -65535 : v >> 10;  // Q15, doubled
        const i32 sq = soft(q, 24575);
        return sq == q ? v >> (kBusShift - 6) : sq << (16 - kBusShift);
    };
    for (u32 i = 0; i < frames; ++i) {
        const i32 l = left[i], r = right[i];
        left[i] = l >= -under && l < over ? l >> (kBusShift - 6) : out(l);
        right[i] = r >= -under && r < over ? r >> (kBusShift - 6) : out(r);
    }
    if (state.fade < kFull)
        for (u32 i = 0; i < frames; ++i) {
            left[i] = multiply(left[i], gain);
            right[i] = multiply(right[i], gain);
        }
    const bool ended = state.frozen ? holding != 0 && state.fade == 0 : state.clock - state.last_loud > kLength;
    if (!ended || heard_in > kQuiet || block_swing(left, right) > kQuiet) {
        state.quiet_blocks = 0;
    } else if (++state.quiet_blocks >= kSleepAfter) {
        // Resting: what still plays is silent, so the grains end here and start afresh on waking.
        state.asleep = 1;
        state.rest[0] = left[frames - 1];
        state.rest[1] = right[frames - 1];
        for (u32 n = 0; n < kGrains; ++n) grains[n].on = 0;
    }
}
}  // namespace earthgrain
