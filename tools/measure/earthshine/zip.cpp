#ifndef MOD
#define MOD 1.0
#endif
#ifndef OFF
#define OFF 0
#endif
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
static opxy::i32 words[earthshine::kBufferWords];
int main() {
    earthshine::State st; earthshine::initialize(st);
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    opxy::i32 p[12] = {q(0.9), q(1), q(0.5), 0, q(1), q(MOD), q(0.5), q(1), 0, 0, 0, 0};
    std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0;
    for (int b = 0; b < 44100 * 6 / 64; ++b) {
        for (int i = 0; i < 64; ++i, ++n) l[i] = r[i] = q(0.2 * std::sin(2 * M_PI * 3000.0 * n / 44100));
        earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (int i = 0; i < 64; ++i) y.push_back(l[i] / 2147483648.0);
    }
    // Power in bands at 3000 +- 689k Hz (k = 1..4) vs at 3000 Hz, over 2..6 s.
    auto pw = [&](double hz) { double re = 0, im = 0; size_t a = 88200, N = 176400;
        for (size_t i = 0; i < N; ++i) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N), ph = 2 * M_PI * hz * i / 44100; re += w * y[a + i] * std::cos(ph); im -= w * y[a + i] * std::sin(ph); }
        return re * re + im * im; };
    double carrier = 0; for (double d = -30; d <= 30; d += 1) carrier += pw(3000 + d);
    double side = 0; for (int k = 1; k <= 4; ++k) for (int sg = -1; sg <= 1; sg += 2) for (double d = -2; d <= 2; d += 0.5) side += pw(3000 + sg * (689.0625 * k + OFF) + d);
    std::printf("Mod %.1f: block-rate sidebands %.1f dB below the tone\n", MOD, 10 * std::log10(carrier / (side + 1e-30)));
}
