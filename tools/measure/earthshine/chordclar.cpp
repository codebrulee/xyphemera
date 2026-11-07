// A low chord (C2 G2 E3, harmonic tones) through the sub only, Mod 0: for each
// note, the share of its sub's energy within +-1 Hz of pitch versus +-12 Hz.
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <initializer_list>
static opxy::i32 words[earthshine::kBufferWords];
int main() {
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    const double notes[3] = {65.41, 98.0, 164.81};
    for (auto& w : words) w = 0;
    earthshine::State st; earthshine::initialize(st);
    opxy::i32 p[12] = {q(0.5), q(0.45), 0, q(1), q(1), 0, q(0.5), q(0.7), q(0.5 / 3), q(0.5 / 3), 0, 0};
    std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0;
    for (int b = 0; b < 44100 * 9 / 64; ++b) {
        for (int i = 0; i < 64; ++i, ++n) {
            double v = 0;
            for (double f : notes) for (int h = 1; h <= 5; ++h) v += std::sin(2 * M_PI * f * h * n / 44100 + h * f) / h;
            l[i] = r[i] = q(0.07 * v);
        }
        earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (int i = 0; i < 64; ++i) y.push_back((l[i] + (double)r[i]) / 4294967296.0);
    }
    auto pw = [&](double hz) { double re = 0, im = 0; size_t a = 44100 * 3, N = 44100 * 6;
        for (size_t i = 0; i < N; ++i) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N), ph = 2 * M_PI * hz * i / 44100; re += w * y[a + i] * std::cos(ph); im -= w * y[a + i] * std::sin(ph); }
        return re * re + im * im; };
    double total = 0;
    for (double f : notes) {
        const double t = f / 2; double in = 0, all = 0;
        for (double d = -12; d <= 12; d += 0.2) { double p2 = pw(t + d); all += p2; if (std::fabs(d) <= 1) in += p2; }
        std::printf("  %6.1f Hz -> %5.1f Hz: %5.1f%% on pitch\n", f, t, 100 * in / all);
        total += 100 * in / all;
    }
    std::printf("  average %.1f%%\n", total / 3);
}
