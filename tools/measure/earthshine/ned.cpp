// Echo density of the reverb's impulse response (Shimmer 0, Mod 0): the
// normalised echo density (Abel & Huang) over 20 ms windows, averaged over
// 50-300 ms and 300-1000 ms. 1 is Gaussian noise (a smooth wash); low is sparse echoes.
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
static opxy::i32 words[earthshine::kBufferWords];
int main() {
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    earthshine::State st; earthshine::initialize(st);
    opxy::i32 p[12] = {q(0.7), q(0.6), q(0.5), 0, q(1), 0, q(0.5), q(0.7), 0, 0, 0, 0};
    std::vector<double> y; opxy::i32 l[64], r[64]; unsigned n = 0; const unsigned at = 8820;
    for (int b = 0; b < 44100 * 2 / 64; ++b) {
        for (int i = 0; i < 64; ++i, ++n) { l[i] = n == at ? q(0.5) : 0; r[i] = 0; }
        earthshine::process(st, opxy::Buffer{words, earthshine::kBufferWords}, l, r, p);
        for (int i = 0; i < 64; ++i) y.push_back(l[i] / 2147483648.0);
    }
    auto ned = [&](double from, double to) {
        double sum = 0; int count = 0; const int W = 882;
        for (size_t c = at + (size_t)(from * 44100); c + W / 2 < at + (size_t)(to * 44100); c += W / 2) {
            double m2 = 0; for (int i = -W / 2; i < W / 2; ++i) m2 += y[c + i] * y[c + i];
            const double sd = std::sqrt(m2 / W); if (sd <= 0) continue;
            int out = 0; for (int i = -W / 2; i < W / 2; ++i) out += std::fabs(y[c + i]) > sd;
            sum += (out / (double)W) / 0.3173; ++count;
        }
        return count ? sum / count : 0;
    };
    std::printf("echo density: early %.2f, late %.2f\n", ned(0.05, 0.3), ned(0.3, 1.0));
}
