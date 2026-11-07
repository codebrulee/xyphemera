// A 110 Hz tone from 1 s, Swell choice SW: when the sub (55 Hz) reaches 50% and
// 90% of its 4-5 s level, minus the same with Swell off.
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
static opxy::i32 words[earthshine::kBufferWords];
static void reach(int sw, double& t50, double& t90) {
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    for (auto& w : words) w = 0;
    earthshine::State st; earthshine::initialize(st);
    opxy::i32 p[12] = {q(0.3), q(0.45), 0, q(1), q(1), 0, q(0.5), q(0.7), q(0.5 / 4), q((sw + 0.5) / 4), q(0.5 / 3), q(0.5 / 3)};
    std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0;
    for (int b = 0; b < 44100 * 5 / 64; ++b) {
        for (int i = 0; i < 64; ++i, ++n) l[i] = r[i] = n > 44100 ? q(0.2 * std::sin(2 * M_PI * 110.0 * n / 44100)) : 0;
        earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (int i = 0; i < 64; ++i) y.push_back(l[i] / 2147483648.0);
    }
    auto amp = [&](size_t a) { double re = 0, im = 0; for (size_t i = 0; i < 2205; ++i) { double ph = 2 * M_PI * 55.0 * (a + i) / 44100; re += y[a + i] * std::cos(ph); im += y[a + i] * std::sin(ph); } return std::sqrt(re * re + im * im); };
    double steady = 0; for (size_t a = 44100 * 4; a < 44100 * 5 - 2205; a += 2205) steady += amp(a) / 19;
    t50 = t90 = -1;
    for (size_t a = 44100; a < 44100 * 5 - 2205; a += 441) {
        const double v = amp(a);
        if (t50 < 0 && v > 0.5 * steady) t50 = (a - 44100) / 44100.0;
        if (t90 < 0 && v > 0.9 * steady) { t90 = (a - 44100) / 44100.0; break; }
    }
}
int main() {
    double b50, b90; reach(0, b50, b90);
    for (int sw = 1; sw < 4; ++sw) { double a, b; reach(sw, a, b); std::printf("Swell %d: 50%% after +%.2f s, 90%% after +%.2f s\n", sw, a - b50, b - b90); }
}
