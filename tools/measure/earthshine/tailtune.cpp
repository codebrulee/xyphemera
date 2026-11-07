// A low note for 1 s through the reverb only (Shimmer 0, Mod MOD), then the pitch
// its tail rings at, 0.3..2.3 s after it stops: cents from the note.
#ifndef MOD
#define MOD 0.0
#endif
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <initializer_list>
static opxy::i32 words[earthshine::kBufferWords];
int main() {
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    double worst = 0, sum = 0; int count = 0;
    for (double f : {41.2, 49.0, 55.0, 61.7, 65.4, 73.4, 82.4, 98.0, 110.0, 130.8}) {
        for (auto& w : words) w = 0;
        earthshine::State st; earthshine::initialize(st);
        opxy::i32 p[12] = {q(0.6), q(0.45), q(0.25), 0, q(1), q(MOD), q(0.5), q(0.7), 0, 0, 0, 0};
        std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0;
        for (int b = 0; b < 44100 * 4 / 64; ++b) {
            for (int i = 0; i < 64; ++i, ++n) l[i] = r[i] = (n > 4410 && n < 4410 + 44100) ? q(0.2 * std::sin(2 * M_PI * f * n / 44100)) : 0;
            earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
            for (int i = 0; i < 64; ++i) y.push_back((l[i] + (double)r[i]) / 4294967296.0);
        }
        auto pw = [&](double hz) { double re = 0, im = 0; size_t a = 4410 + 44100 + 13230, N = 88200;
            for (size_t i = 0; i < N; ++i) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N), ph = 2 * M_PI * hz * i / 44100; re += w * y[a + i] * std::cos(ph); im -= w * y[a + i] * std::sin(ph); }
            return re * re + im * im; };
        double best = 0, bp = -1;
        for (double c = -80; c <= 80; c += 2) { double p2 = pw(f * std::pow(2, c / 1200)); if (p2 > bp) { bp = p2; best = c; } }
        std::printf("  %6.1f Hz: tail at %+4.0f cents\n", f, best);
        worst = std::fmax(worst, std::fabs(best)); sum += std::fabs(best); ++count;
    }
    std::printf("  mean %.1f cents, worst %.0f\n", sum / count, worst);
}
