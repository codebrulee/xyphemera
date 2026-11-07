// A held 220 Hz tone, sub shimmer only, Swell SW: energy in sidebands at
// 110 +- 689 Hz (block-rate ring) relative to the sub at 110 Hz.
#ifndef DELAYSLOT
#define DELAYSLOT q(0.5 / 4)
#endif
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
static opxy::i32 words[earthshine::kBufferWords];
int main() {
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    for (int sw = 0; sw < 4; ++sw) {
        for (auto& w : words) w = 0;
        earthshine::State st; earthshine::initialize(st);
        opxy::i32 p[12] = {q(0.3), q(0.45), 0, q(1), q(1), 0, q(0.5), q(0.7), DELAYSLOT, q((sw + 0.5) / 4), q(0.5 / 3), q(0.5 / 3)};
        std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0;
        for (int b = 0; b < 44100 * 5 / 64; ++b) {
            for (int i = 0; i < 64; ++i, ++n) l[i] = r[i] = q(0.2 * std::sin(2 * M_PI * 220.0 * n / 44100));
            earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
            for (int i = 0; i < 64; ++i) y.push_back(l[i] / 2147483648.0);
        }
        auto pw = [&](double hz) { double re = 0, im = 0; size_t a = 88200, N = 132300;
            for (size_t i = 0; i < N; ++i) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N), ph = 2 * M_PI * hz * i / 44100; re += w * y[a + i] * std::cos(ph); im -= w * y[a + i] * std::sin(ph); }
            return re * re + im * im; };
        double sub = 0, ring = 0;
        for (double d = -2; d <= 2; d += 0.5) { sub += pw(110 + d); ring += pw(110 + 689.0625 + d) + pw(689.0625 - 110 + d); }
        std::printf("Swell %d: block-rate ring %.1f dB below the sub\n", sw, 10 * std::log10(sub / (ring + 1e-30)));
    }
}
