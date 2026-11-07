// Renders a test phrase through an effect's fixed-point code (as on the device)
// to 24-bit WAVs: <out>_wet.wav and <out>_mix.wav (the dry at 0.7 plus the wet).
//
//   g++ -O2 -std=c++17 -I<open-xy>/sdk/include -I<effect> -DNS=earthshine \
//       -DPARAMETERS="..." [-DTRANSPOSE=-12] tools/render/abrender.cpp -o render
//   ./render <out> <gain>
//
// PARAMETERS: the twelve Q31 words process() gets. Use one gain across the
// files of an A/B so their levels compare. The phrase: three pad chords, a
// plucked melody, then a tail.
#include "fx.cpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#ifndef TRANSPOSE
#define TRANSPOSE 0
#endif
static opxy::i32 words[NS::kBufferWords];
const double kFs = 44100;
static std::vector<float> L, R;
static void phrase() {
    int total = int(kFs * 24); L.assign(total, 0); R.assign(total, 0);
    auto mtof = [](double m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); };
    const int chords[3][4] = {{48, 55, 62, 64}, {45, 52, 59, 60}, {41, 48, 55, 57}};
    for (int k = 0; k < 3; ++k) {
        int start = int(kFs * 2.2 * k), len = int(kFs * 1.8);
        for (int n = 0; n < 4; ++n) { double f0 = mtof(chords[k][n] + TRANSPOSE);
            for (int i = 0; i < len; ++i) { double t = i / kFs, env = std::min(1.0, t / 0.08) * std::min(1.0, (len - i) / (0.3 * kFs)), v = 0;
                for (int h = 1; h <= 12; ++h) v += std::sin(2 * M_PI * f0 * h * t) / (h * h * 0.6 + h * 0.4);
                L[start + i] += 0.06 * env * v; R[start + i] += 0.06 * env * v; } }
    }
    const int melody[6] = {72, 76, 79, 74, 71, 67};
    for (int k = 0; k < 6; ++k) { int start = int(kFs * (7.0 + 0.45 * k)); double f0 = mtof(melody[k] + TRANSPOSE), pan = 0.3 + 0.4 * (k & 1);
        for (int i = 0; i < int(kFs * 1.5); ++i) { double t = i / kFs, v = 0;
            for (int h = 1; h <= 10; ++h) v += std::sin(2 * M_PI * f0 * h * t) * std::exp(-t * (2.0 + 1.5 * h)) / h;
            v *= std::min(1.0, t / 0.002); L[start + i] += 0.2 * v * (1 - pan); R[start + i] += 0.2 * v * pan; } }
}
static void wav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, double g) {
    FILE* f = std::fopen(path.c_str(), "wb"); uint32_t n = l.size(), data = n * 6, rate = 44100, br = rate * 6, riff = 36 + data, fl = 16; uint16_t pcm = 1, ch = 2, al = 6, bits = 24;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fl, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&al, 2, 1, f); std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i) for (float v : {l[i], r[i]}) { double x = std::fmax(-1, std::fmin(1, v * g)); int32_t q = int32_t(x * 8388607); uint8_t b[3] = {uint8_t(q), uint8_t(q >> 8), uint8_t(q >> 16)}; std::fwrite(b, 1, 3, f); }
    std::fclose(f);
}
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: render <out> <gain>\n"); return 1; }
    // args: out gain
    const double gain = atof(argv[2]);
    phrase();
    for (auto& w : words) w = 0;
    NS::State st; NS::initialize(st);
    auto q = [](double v) { return (opxy::i32)(v * 2147483647.0); };
    (void)q;
    const opxy::i32 p[12] = {PARAMETERS};
    std::vector<float> wl, wr; opxy::i32 bl[64], br[64]; double secs = 0, peak = 0;
    for (size_t b = 0; b + 64 <= L.size(); b += 64) {
        for (int i = 0; i < 64; ++i) { bl[i] = q(L[b + i] * 0.5); br[i] = q(R[b + i] * 0.5); }
        auto t0 = std::chrono::steady_clock::now();
        NS::process(st, opxy::Buffer{words, NS::kBufferWords}, bl, br, p);
        secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < 64; ++i) { wl.push_back(bl[i] / 2147483648.0); wr.push_back(br[i] / 2147483648.0); peak = std::fmax(peak, std::fabs(wl.back())); }
    }
    std::vector<float> ml(wl.size()), mr(wl.size());
    for (size_t i = 0; i < wl.size(); ++i) { ml[i] = 0.7 * L[i] + gain * wl[i]; mr[i] = 0.7 * R[i] + gain * wr[i]; }
    wav(std::string(argv[1]) + "_wet.wav", wl, wr, gain);
    wav(std::string(argv[1]) + "_mix.wav", ml, mr, 1);
    std::printf("%s: wet peak %.3f, host time %.1f ms\n", argv[1], peak, secs * 1000);
}
