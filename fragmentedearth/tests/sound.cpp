// Sound checks for the granular looper, compiled for the host and run by
// python sdk.py test --machine <folder>.
#include "../fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check(const char* what, bool ok) {
    if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
}

static opxy::i32 words[fragmentedearth::kBufferWords];
const double kFs = 44100;
const double kPi = 3.14159265358979;

static opxy::i32 q31(double x) { return static_cast<opxy::i32>(x * 2147483647.0); }

struct Settings {
    double activity = 0.5, size = 0.5, shape = 0.5, drift = 0.3, repeats = 0.2, droop = 0, space = 0;
    unsigned pitch = 0, mode = 0, motion = 0, sync = 0, bend = 0, hold = 0;  // enum choices (hold: 0 forever .. 8 off)
};

static void words_for(const Settings& s, opxy::i32 (&p)[12]) {
    for (unsigned i = 0; i < 12; ++i) p[i] = 0;
    p[0] = q31(s.activity); p[1] = q31(s.size); p[2] = q31(s.shape);
    p[3] = q31((s.pitch + 0.5) / 8);  // an enum stores the middle of its choice's range
    p[4] = q31(s.drift); p[5] = q31(s.repeats); p[6] = q31(s.droop); p[7] = q31(s.space);
    p[8] = q31((s.mode + 0.5) / 4);   // the Shift row: Mode, then Motion
    p[9] = q31((s.motion * 2 + s.bend + 0.5) / 6);  // Motion: its shape, bending down or up
    p[10] = q31((s.sync + 0.5) / 2);  // Sync: free, tempo
    p[11] = q31((s.hold + 0.5) / 9);  // Hold: forever, 64 s .. 1 s, off
}

// Runs `seconds` of input(n) (both sides) and returns the wet, (left + right) / 2.
template <class Input>
static std::vector<double> run(const Settings& s, double seconds, Input input, bool garbage = false) {
    for (opxy::u32 i = 0; i < fragmentedearth::kBufferWords; ++i)
        words[i] = garbage ? static_cast<opxy::i32>(i * 2654435761u) : 0;
    fragmentedearth::State state;
    fragmentedearth::initialize(state);
    opxy::i32 p[12];
    words_for(s, p);
    std::vector<double> out;
    const unsigned blocks = static_cast<unsigned>(seconds * kFs / opxy::block_frames);
    opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
    for (unsigned b = 0; b < blocks; ++b) {
        // At the send bus's level, 2^10 below full scale; the output read back at full scale.
        for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = input(b * opxy::block_frames + i) / 1024;
        fragmentedearth::process(state, opxy::Buffer{words, fragmentedearth::kBufferWords}, opxy::Transport{1200, b * 64u * 96u / 22050u, true}, l, r, p);
        for (unsigned i = 0; i < opxy::block_frames; ++i) out.push_back((l[i] + static_cast<double>(r[i])) * 1024 / 4294967296.0);
    }
    return out;
}

static double rms(const std::vector<double>& x, double from, double to) {
    double sum = 0; unsigned n = 0;
    for (unsigned i = static_cast<unsigned>(from * kFs); i < x.size() && i < to * kFs; ++i, ++n) sum += x[i] * x[i];
    return n ? std::sqrt(sum / n) : 0;
}

// A 220 Hz sine for 5 s, then quiet: the second difference of the output, far above a pure
// sine's, marks a click the engine made.
static int clicks(const std::vector<double>& out, double top) {
    const double w = 2 * kPi * 220 * top / kFs;
    int found = 0, last = -100000;
    for (int n = 2; n < static_cast<int>(out.size()); ++n) {
        const double d2 = std::fabs(out[n] - 2 * out[n - 1] + out[n - 2]);
        double a = 0;
        for (int k = -200; k <= 200; k += 10) {
            const int m = n + k;
            if (m >= 0 && m < static_cast<int>(out.size())) a = std::fmax(a, std::fabs(out[m]));
        }
        if (d2 > 3 * a * w * w + 2e-3 && n - last > 220) { ++found; last = n; }
    }
    return found;
}

int main() {
    auto sine = [](unsigned n) {
        // In and out over 50 ms: Space now carries the playing too, so a tone starting at full
        // level would be heard starting in it.
        const double fade = n < 2205 ? n / 2205.0 : n < kFs * 5 ? 1.0 : std::fmax(0.0, 1 - (n - kFs * 5) / 2205.0);
        return q31(0.25 * fade * std::sin(2 * kPi * 220 * n / kFs));
    };
    auto quiet = [](unsigned) { return 0; };

    // Outside a send there is no buffer: silence.
    {
        fragmentedearth::State state;
        fragmentedearth::initialize(state);
        opxy::i32 p[12];
        words_for(Settings(), p);
        opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
        bool silent = true;
        for (unsigned b = 0; b < 100; ++b) {
            for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = q31(0.5);
            fragmentedearth::process(state, opxy::Buffer{nullptr, 0}, opxy::Transport{0, 0, false}, l, r, p);
            for (unsigned i = 0; i < opxy::block_frames; ++i) silent = silent && !l[i] && !r[i];
        }
        check("silence outside a send", silent);
    }

    // A state and grain pool left by another build: whatever they hold, nothing outside the
    // recording is read and no garbage is heard.
    for (unsigned seed = 1; seed <= 20; ++seed) {
        for (opxy::u32 i = 0; i < fragmentedearth::kBufferWords; ++i) words[i] = static_cast<opxy::i32>((i + seed) * 2654435761u);
        fragmentedearth::State state;
        opxy::u32* raw = reinterpret_cast<opxy::u32*>(&state);
        for (unsigned i = 0; i < sizeof state / 4; ++i) raw[i] = (i + 7 * seed) * 2246822519u;
        if (seed & 1) state.ready = 1;
        opxy::i32 p[12];
        words_for(Settings(), p);
        opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
        double peak = 0;
        for (unsigned b = 0; b < 400; ++b) {
            for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = 0;
            fragmentedearth::process(state, opxy::Buffer{words, fragmentedearth::kBufferWords}, opxy::Transport{1200, 0, true}, l, r, p);
            for (unsigned i = 0; i < opxy::block_frames; ++i) peak = std::fmax(peak, std::fabs(double(l[i])));
        }
        check("a stale state and grain pool are not heard", peak == 0);
    }

    for (unsigned mode = 0; mode < 4; ++mode) {
        Settings s;
        s.mode = mode;
        // The buffer is never cleared: what a previous effect left must never be heard.
        const std::vector<double> stale = run(s, 3, quiet, true);
        const char* const names[4] = {"cloud", "stretch", "spring", "slice"};
        char what[96];
        std::snprintf(what, sizeof what, "%s: silence in, silence out over a dirty buffer", names[mode]);
        check(what, rms(stale, 0, 3) == 0);

        // Playing, then quiet: grains while playing, and the phrase held as a drone after.
        const std::vector<double> out = run(s, 12, sine);
        const double playing = rms(out, 1, 5), held = rms(out, 8, 12);
        std::printf("%s: playing %.1f dB, held %.1f dB\n", names[mode],
                    20 * std::log10(playing + 1e-12), 20 * std::log10(held + 1e-12));
        std::snprintf(what, sizeof what, "%s: grains while playing", names[mode]);
        check(what, playing > 0.02);
        std::snprintf(what, sizeof what, "%s: holds the phrase", names[mode]);
        check(what, held > 0.02);
        double peak = 0;
        for (double x : out) peak = std::fmax(peak, std::fabs(x));
        check("output stays below the bus's full scale", peak < 0.99);
    }

    // Hold: off holds nothing, so the grains run out once the recording goes quiet; 2 s fades
    // the held drone away over about 2 s.
    {
        Settings s;
        s.hold = 8;
        const std::vector<double> out = run(s, 12, sine);
        check("hold off: the grains run out after the playing stops", rms(out, 9, 12) < 0.0005);
        s.hold = 6;
        const std::vector<double> held = run(s, 14, sine);
        check("hold 2 s: the drone sounds as it is held", rms(held, 6.3, 6.8) > 0.01);
        check("hold 2 s: the drone has faded a few seconds later", rms(held, 11, 14) < 0.0005);
    }

    // Space: a tail after the grains, which dies away once they stop.
    {
        Settings s;
        s.hold = 8;
        s.space = 1;
        const std::vector<double> out = run(s, 16, sine);
        const double dry = rms(run(Settings{}, 16, sine), 2, 4);
        std::printf("space full: playing %.1f dB (without %.1f dB), 9-10 s %.1f dB\n", 20 * std::log10(rms(out, 2, 4) + 1e-12),
                    20 * std::log10(dry + 1e-12), 20 * std::log10(rms(out, 9, 10) + 1e-12));
        check("space: sounds while playing", rms(out, 2, 4) > 0.02);
        check("space: its tail has died away", rms(out, 14, 16) < 0.0005);
    }

    // Spring with long grains: live, its grains repeat one moment in phase, so they must not
    // pile up into the clipper; held, it repeats the phrase rather than the quiet at its end.
    {
        Settings s;
        s.mode = 2; s.size = 0.8; s.shape = 0.8; s.repeats = 0; s.drift = 0.8;
        const std::vector<double> out = run(s, 16, sine);
        double peak = 0;
        for (unsigned n = 0; n < static_cast<unsigned>(5 * kFs); ++n) peak = std::fmax(peak, std::fabs(out[n]));
        std::printf("spring long grains: peak %.2f playing, held %.1f dB\n", peak, 20 * std::log10(rms(out, 8, 16) + 1e-12));
        check("spring: long grains stay clear of the clipper", peak < 0.7);
        for (int sec = 8; sec < 16; ++sec) check("spring: held, it keeps repeating", rms(out, sec, sec + 1) > 0.005);
    }

    // No clicks: grains of every length, both directions, Repeats rewriting the held phrase.
    struct Case { const char* name; Settings s; double top; };
    Case cases[15];
    cases[0] = {"cloud plain, some space", Settings(), 1};
    cases[0].s.space = 0.4;
    cases[1] = {"cloud short plucks", Settings(), 1};
    cases[1].s.activity = 0.85; cases[1].s.size = 0.25; cases[1].s.shape = 0.2;
    cases[2] = {"cloud reverse, high repeats", Settings(), 1};
    cases[2].s.pitch = 6; cases[2].s.repeats = 0.9;
    cases[3] = {"stretch plain", Settings(), 1};
    cases[3].s.mode = 1;
    cases[4] = {"stretch still", Settings(), 1};
    cases[4].s.mode = 1; cases[4].s.activity = 0;
    cases[5] = {"stretch octaves, high repeats", Settings(), 2};
    cases[5].s.mode = 1; cases[5].s.pitch = 5; cases[5].s.repeats = 0.8; cases[5].s.drift = 0.7;
    // Droop, Motion, Sync and Bend in Cloud and Stretch.
    cases[6] = {"cloud droop up, harmonics, long grains", Settings(), 8};
    cases[6].s.droop = 1; cases[6].s.bend = 1; cases[6].s.pitch = 7; cases[6].s.size = 1;
    cases[7] = {"cloud on tempo, droop down", Settings(), 1};
    cases[7].s.sync = 1; cases[7].s.droop = 0.6; cases[7].s.activity = 0.7;
    cases[8] = {"stretch on tempo, expand", Settings(), 1};
    cases[8].s.mode = 1; cases[8].s.sync = 1; cases[8].s.motion = 1; cases[8].s.droop = 0.5;
    cases[9] = {"spring droop up, harmonics, long grains", Settings(), 8};
    cases[9].s.mode = 2; cases[9].s.droop = 1; cases[9].s.bend = 1; cases[9].s.pitch = 7; cases[9].s.size = 1;
    cases[10] = {"slice plain, full space", Settings(), 1};
    cases[10].s.mode = 3; cases[10].s.space = 1;
    cases[11] = {"slice fast, reversed, wandering", Settings(), 1};
    cases[11].s.mode = 3; cases[11].s.activity = 0.9; cases[11].s.pitch = 6; cases[11].s.drift = 1; cases[11].s.size = 0.2;
    cases[12] = {"slice on tempo", Settings(), 1};
    cases[12].s.mode = 3; cases[12].s.sync = 1; cases[12].s.activity = 0.6;
    cases[13] = {"spring long grains, slow, high repeats", Settings(), 1};
    cases[13].s.mode = 2; cases[13].s.size = 0.8; cases[13].s.shape = 0.8; cases[13].s.repeats = 0.7; cases[13].s.drift = 0;
    cases[14] = {"spring long grains, fast cycle", Settings(), 1};
    cases[14].s.mode = 2; cases[14].s.size = 0.8; cases[14].s.shape = 0.2; cases[14].s.repeats = 0.3;
    for (const Case& c : cases) {
        const int n = clicks(run(c.s, 16, sine), c.top);
        if (n) std::printf("%s: %d clicks\n", c.name, n);
        check("no clicks", n == 0);
    }
    return failures ? 1 : 0;
}
