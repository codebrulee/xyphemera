// Sound checks for Earthshine, compiled for the host and run by
// python sdk.py test --machine <folder>.
#include "../fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check(const char* what, bool ok) {
    if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
}

static opxy::i32 words[earthshine::kBufferWords];
const double kFs = 44100;

static opxy::i32 q31(double x) { return static_cast<opxy::i32>(x * 2147483647.0); }

struct Settings {
    double decay = 0.55, tone = 0.45, light = 0.25, shimmer = 0.6, dry = 1, mod = 0, fine = 0.5, width = 0.7;
    unsigned sub = 0, up = 0;  // shift-row choices
};

static void words_for(const Settings& s, opxy::i32 (&p)[12]) {
    const double v[8] = {s.decay, s.tone, s.light, s.shimmer, s.dry, s.mod, s.fine, s.width};
    for (unsigned i = 0; i < 12; ++i) p[i] = 0;
    for (unsigned i = 0; i < 8; ++i) p[i] = q31(v[i]);
    // An enum stores the middle of its choice's range.
    // The Shift row: Delay and Swell (first choices: none), then Sub and Up.
    p[8] = q31(0.5 / 4);
    p[9] = q31(0.5 / 4);
    p[10] = q31((s.sub + 0.5) / 3);
    p[11] = q31((s.up + 0.5) / 3);
}

// Runs `seconds` of input(n) (both sides) and returns the wet, left + right.
template <class Input>
static std::vector<double> run(const Settings& s, double seconds, Input input, bool garbage = false) {
    for (opxy::u32 i = 0; i < earthshine::kBufferWords; ++i)
        words[i] = garbage ? static_cast<opxy::i32>(i * 2654435761u) : 0;
    earthshine::State state;
    earthshine::initialize(state);
    opxy::i32 p[12];
    words_for(s, p);
    std::vector<double> out;
    const unsigned blocks = static_cast<unsigned>(seconds * kFs / opxy::block_frames);
    opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = input(b * opxy::block_frames + i);
        earthshine::process(state, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (unsigned i = 0; i < opxy::block_frames; ++i) out.push_back((l[i] + static_cast<double>(r[i])) / 4294967296.0);
    }
    return out;
}

// As run, but hands each output frame's left and right to `each`.
template <class Input, class Each>
static void run_stereo(const Settings& s, double seconds, Input input, Each each) {
    for (opxy::u32 i = 0; i < earthshine::kBufferWords; ++i) words[i] = 0;
    earthshine::State state;
    earthshine::initialize(state);
    opxy::i32 p[12];
    words_for(s, p);
    const unsigned blocks = static_cast<unsigned>(seconds * kFs / opxy::block_frames);
    opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = input(b * opxy::block_frames + i);
        earthshine::process(state, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (unsigned i = 0; i < opxy::block_frames; ++i) each(l[i], r[i]);
    }
}

static double rms(const std::vector<double>& x, double from, double to) {
    const size_t a = static_cast<size_t>(from * kFs), b = static_cast<size_t>(to * kFs);
    double sum = 0;
    for (size_t i = a; i < b && i < x.size(); ++i) sum += x[i] * x[i];
    return std::sqrt(sum / (b - a));
}

static double peak(const std::vector<double>& x, double from, double to) {
    double m = 0;
    for (size_t i = static_cast<size_t>(from * kFs); i < static_cast<size_t>(to * kFs) && i < x.size(); ++i)
        m = std::fmax(m, std::fabs(x[i]));
    return m;
}

// Power at hz over [from, to) seconds, Hann windowed.
static double power(const std::vector<double>& x, double hz, double from, double to) {
    const size_t a = static_cast<size_t>(from * kFs), n = static_cast<size_t>((to - from) * kFs);
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n), ph = 2 * M_PI * hz * i / kFs;
        re += w * x[a + i] * std::cos(ph);
        im -= w * x[a + i] * std::sin(ph);
    }
    return re * re + im * im;
}

// The strongest frequency within +-60 cents of `target`, at 0.5-cent steps.
static double strongest_near(const std::vector<double>& x, double target, double from, double to) {
    double best = target, best_power = -1;
    for (double c = -60; c <= 60; c += 0.5) {
        const double hz = target * std::pow(2.0, c / 1200);
        const double p = power(x, hz, from, to);
        if (p > best_power) { best_power = p; best = hz; }
    }
    return best;
}

static double cents(double hz, double target) { return 1200 * std::log2(hz / target); }

int main() {
    const auto silence = [](unsigned) { return 0; };
    // After the 14 silent blocks (about 20 ms) that clear the buffer at start.
    const auto impulse = [](unsigned n) { return n == 4410 ? q31(0.5) : 0; };
    const auto sine = [](double hz) {
        return [hz](unsigned n) { return q31(0.1 * std::sin(2 * M_PI * hz * n / kFs)); };
    };

    // Outside a send there is no buffer: silence.
    {
        earthshine::State state;
        earthshine::initialize(state);
        opxy::i32 p[12];
        words_for(Settings(), p);
        opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
        for (unsigned i = 0; i < opxy::block_frames; ++i) l[i] = r[i] = q31(0.5);
        earthshine::process(state, opxy::Buffer{nullptr, 0}, l, r, p);
        bool quiet = true;
        for (unsigned i = 0; i < opxy::block_frames; ++i) quiet = quiet && !l[i] && !r[i];
        check("silent without a buffer", quiet);
    }

    // Whatever the scratch memory held, silence in is silence out.
    check("silence from a stale buffer", peak(run(Settings(), 1.0, silence, true), 0, 1.0) == 0);

    // An impulse rings on, and longer with more Decay.
    Settings plain;
    plain.shimmer = 0;
    plain.decay = 0.3;
    const std::vector<double> short_tail = run(plain, 6, impulse);
    plain.decay = 0.8;
    const std::vector<double> long_tail = run(plain, 6, impulse);
    check("an impulse rings", rms(short_tail, 0.2, 0.7) > 1e-4);
    check("the tail decays", rms(short_tail, 4, 5) < rms(short_tail, 0.5, 1.5) * 0.1);
    check("more Decay rings longer", rms(long_tail, 4, 5) > rms(short_tail, 4, 5) * 4);

    // Tuning: a 220 Hz sine through each interval lands on pitch, within 3 cents
    // (the shifters alone hold +-0.5; the shimmer loop adds a little spread).
    // Fine's stretched octave up stacks partials that share no period as it
    // recirculates, so its splices can't all align: it is allowed 5 cents.
    struct Case { const char* what; double light; unsigned sub, up; double fine, ratio, tolerance; };
    const Case cases[] = {
        {"the sub octave is in tune", 0, 0, 0, 0.5, 0.5, 3},
        {"two octaves down are in tune", 0, 1, 0, 0.5, 0.25, 3},
        {"a fifth down is in tune", 0, 2, 0, 0.5, std::pow(2.0, -7.0 / 12), 3},
        {"the octave up is in tune", 1, 0, 0, 0.5, 2.0, 3},
        {"the tank itself keeps pitch", 1, 0, 0, 0.5, 1.0, 3},
        {"Fine stretches the octave up by 25 cents", 1, 0, 0, 1.0, 2.0 * std::pow(2.0, 25.0 / 1200), 5},
        {"Fine stretches the sub octave down by 25 cents", 0, 0, 0, 1.0, 0.5 * std::pow(2.0, -25.0 / 1200), 3},
    };
    for (const Case& c : cases) {
        Settings s;
        s.shimmer = 1;
        s.light = c.light;
        s.sub = c.sub;
        s.up = c.up;
        s.fine = c.fine;
        s.decay = 0.5;
        const std::vector<double> out = run(s, 5, sine(220));
        const double target = 220 * c.ratio;
        const double found = strongest_near(out, target, 2, 5);
        if (std::fabs(cents(found, target)) > c.tolerance) std::printf("  %s: %.2f Hz, %+.1f cents\n", c.what, found, cents(found, target));
        check(c.what, std::fabs(cents(found, target)) <= c.tolerance);
    }

    // Shimmer at 0 adds no shifted layer; at full it does.
    {
        Settings s;
        s.shimmer = 0;
        s.light = 0;
        const std::vector<double> off = run(s, 4, sine(220));
        s.shimmer = 1;
        const std::vector<double> on = run(s, 4, sine(220));
        const double off_ratio = power(off, 110, 2, 4) / power(off, 220, 2, 4);
        const double on_ratio = power(on, 110, 2, 4) / power(on, 220, 2, 4);
        check("no sub without Shimmer", off_ratio < 1e-4);
        check("a sub with Shimmer", on_ratio > 0.05);
    }

    // The halo (Up's rise and fall) colours the tail without shifting it: at
    // the sun end it adds to the wet (a 1 kHz tone, inside its 250 Hz .. 6 kHz)
    // and the tone stays where it is.
    {
        Settings s;
        s.light = 1;
        s.shimmer = 0;
        const std::vector<double> plain = run(s, 6, sine(1000));
        s.shimmer = 1;
        for (unsigned up = 1; up <= 2; ++up) {
            s.up = up;
            const std::vector<double> halo = run(s, 6, sine(1000));
            double changed = 0, total = 0;
            for (size_t n = 44100 * 2; n < halo.size(); ++n) {
                changed += (halo[n] - plain[n]) * (halo[n] - plain[n]);
                total += plain[n] * plain[n];
            }
            const double found = strongest_near(halo, 1000, 2, 6);
            if (std::fabs(cents(found, 1000)) > 3) std::printf("  halo %u: %.2f Hz\n", up, found);
            check(up == 1 ? "the rising halo adds to the wet" : "the falling halo adds to the wet", changed > total * 0.01);
            check(up == 1 ? "the rising halo keeps the note's pitch" : "the falling halo keeps the note's pitch", std::fabs(cents(found, 1000)) <= 3);
        }
    }

    // Width 0 is mono; full width keeps the sides apart.
    {
        Settings s;
        s.width = 0;
        bool mono = true;
        run_stereo(s, 3, [](unsigned n) { return n == 4410 ? q31(0.5) : 0; }, [&](opxy::i32 l, opxy::i32 r) {
            if (l > r + 2 || r > l + 2) mono = false;
        });
        check("Width 0 is mono", mono);
        s.width = 0.7;
        double diff = 0;
        run_stereo(s, 3, [](unsigned n) { return n == 4410 ? q31(0.5) : 0; }, [&](opxy::i32 l, opxy::i32 r) {
            diff += std::fabs((l - static_cast<double>(r)) / 2147483648.0);
        });
        check("full Width is stereo", diff > 1);
    }

    // Width above 70% sways the wet left and right, subtly at first. The sway
    // only scales the output, so each side's level is compared with the same
    // run at 70%, a quarter second at a time: the balance's swing in dB.
    {
        const auto levels = [](double width) {
            Settings s;
            s.width = width;
            s.shimmer = 0;
            s.decay = 0.9;
            std::vector<double> sides;
            double l = 0, r = 0;
            unsigned n = 0;
            run_stereo(s, 12, [](unsigned i) { return static_cast<opxy::i32>((i * 2654435761u) & 0x0fffffff) - 0x08000000; },
                       [&](opxy::i32 a, opxy::i32 b) {
                           if (++n < 2 * 44100) return;
                           l += static_cast<double>(a) * a;
                           r += static_cast<double>(b) * b;
                           if (n % 11025 == 0) {
                               sides.push_back(l + 1);
                               sides.push_back(r + 1);
                               l = r = 0;
                           }
                       });
            return sides;
        };
        const std::vector<double> open = levels(0.7);
        const auto swing = [&](double width) {
            const std::vector<double> w = levels(width);
            double lo = 1e9, hi = -1e9;
            for (size_t i = 0; i + 1 < w.size(); i += 2) {
                const double db = 10 * std::log10((w[i] / open[i]) / (w[i + 1] / open[i + 1]));
                lo = std::fmin(lo, db);
                hi = std::fmax(hi, db);
            }
            return hi - lo;
        };
        const double slight = swing(0.8), full = swing(1);
        if (!(slight > 0.2 && slight < 4 && full > 8 && full < 20))
            std::printf("  sway: Width 80%% %.1f dB, 100%% %.1f dB\n", slight, full);
        check("Width 80% sways a little", slight > 0.2 && slight < 4);
        check("Width 100% sways clearly", full > 8 && full < 20);
    }

    // Swell keeps to its labels: after a note starts, the sub reaches 90% of
    // its held level about 0.5, 1 and 2 s later than with Swell off.
    {
        const auto reach = [](unsigned swell) {
            Settings s;
            s.shimmer = 1;
            s.light = 0;
            s.decay = 0.3;
            opxy::i32 p[12];
            words_for(s, p);
            p[9] = q31((swell + 0.5) / 4);
            for (opxy::u32 i = 0; i < earthshine::kBufferWords; ++i) words[i] = 0;
            earthshine::State state;
            earthshine::initialize(state);
            std::vector<double> out;
            opxy::i32 l[opxy::block_frames], r[opxy::block_frames];
            for (unsigned b = 0; b < 5 * 44100 / opxy::block_frames; ++b) {
                for (unsigned i = 0; i < opxy::block_frames; ++i) {
                    const unsigned n = b * opxy::block_frames + i;
                    l[i] = r[i] = n > 44100 ? q31(0.2 * std::sin(2 * M_PI * 110.0 * n / kFs)) : 0;
                }
                earthshine::process(state, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
                for (unsigned i = 0; i < opxy::block_frames; ++i) out.push_back(l[i] / 2147483648.0);
            }
            const auto amplitude = [&](size_t a) {
                double re = 0, im = 0;
                for (size_t i = 0; i < 2205; ++i) {
                    const double ph = 2 * M_PI * 55.0 * (a + i) / kFs;
                    re += out[a + i] * std::cos(ph);
                    im += out[a + i] * std::sin(ph);
                }
                return std::sqrt(re * re + im * im);
            };
            double held = 0;
            for (size_t a = 4 * 44100; a < 5 * 44100 - 2205; a += 2205) held += amplitude(a) / 19;
            for (size_t a = 44100; a < 5 * 44100 - 2205; a += 441)
                if (amplitude(a) > 0.9 * held) return (a - 44100) / kFs;
            return 99.0;
        };
        const double off = reach(0), half = reach(1) - off, one = reach(2) - off, two = reach(3) - off;
        if (!(half > 0.3 && half < 0.8 && one > 0.75 && one < 1.4 && two > 1.6 && two < 2.6))
            std::printf("  swell: %.2f, %.2f, %.2f s\n", half, one, two);
        check("Swell keeps to its labels", half > 0.3 && half < 0.8 && one > 0.75 && one < 1.4 && two > 1.6 && two < 2.6);
    }

    // Mod glides the reads: a steady tone gains no buzz at the block rate
    // (44,100 / 64 Hz) however hard Mod drifts the lines. The drift itself
    // spreads the tone (out to about 850 Hz at full), so the block-rate bands
    // are compared with their neighbours 100 Hz either side; a jump at each
    // block would stand out as a peak.
    {
        Settings s;
        s.shimmer = 0;
        s.mod = 1;
        s.tone = 1;
        s.decay = 0.9;
        const std::vector<double> out = run(s, 6, sine(3000));
        double buzz = 0, beside = 0;
        for (int k = 1; k <= 4; ++k)
            for (int side = -1; side <= 1; side += 2)
                for (double d = -2; d <= 2; d += 0.5) {
                    buzz += power(out, 3000 + side * 689.0625 * k + d, 2, 6);
                    beside += (power(out, 3000 + side * (689.0625 * k - 100) + d, 2, 6) +
                               power(out, 3000 + side * (689.0625 * k + 100) + d, 2, 6)) / 2;
                }
        const double above = 10 * std::log10((buzz + 1e-30) / (beside + 1e-30));
        if (above > 6) std::printf("  block-rate bands %.1f dB above the bands beside them\n", above);
        check("Mod at full doesn't buzz at the block rate", above <= 6);
    }

    // The heaviest settings stay bounded and die away once the input stops.
    const double lights[3] = {0, 0.5, 1};
    for (unsigned up = 0; up < 3; up += 2)
    for (double light : lights) {
        Settings s;
        s.decay = 1;
        s.shimmer = 1;
        s.light = light;
        s.mod = 1;
        s.tone = 1;
        s.up = up;
        const std::vector<double> out = run(s, 40, [](unsigned n) {
            return n < 44100 ? static_cast<opxy::i32>((n * 2654435761u) & 0x3fffffff) - 0x20000000 : 0;
        });
        const double early = rms(out, 1.5, 3), late = rms(out, 37, 40);
        if (!(late < early * 0.5)) std::printf("  light %.1f: early %.3g late %.3g\n", light, early, late);
        check("full Decay and Shimmer die away", late < early * 0.5);
        check("full Decay and Shimmer stay below full scale", peak(out, 0, 40) < 0.99);
    }

    if (failures) return 1;
    std::puts("earthshine ok");
    return 0;
}
