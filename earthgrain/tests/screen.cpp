// Screen checks for the granular looper, run by python sdk.py test --machine <folder>:
// every frame must pass the SDK's check (the device refuses an invalid frame whole and
// keeps showing its artwork), whether the dust is up from the start or not.
#include "../display.cpp"
#include <cstdio>
#include <cstring>

static earthgrain::Frame frame;

int main() {
    int failures = 0;
    for (int start = 0; start < 2; ++start)
        for (unsigned mode = 0; mode < 4; ++mode) {
            earthgrain::DisplayState state;
            earthgrain::display_reset(state);
            opxy::draw::Input input{};
            input.elapsed = 34;  // 30 frames a second, Q10
            input.track = 14;
            for (unsigned i = 0; i < 4; ++i) input.shown[i] = static_cast<opxy::u8>(i);
            input.choices[0] = static_cast<opxy::u8>(mode);
            for (int f = 0; f < 360; ++f) {
                const double t = f / 30.0;
                input.slots[0] = start || (t > 1 && t < 8) ? 0x60000000 : 0;  // Activity
                input.slots[1] = input.slots[2] = 0x40000000;                 // Size, Shape
                input.slots[4] = t > 4 && t < 8 ? 0x70000000 : 0;             // Drift
                input.slots[7] = 0x26666666;                                  // Space 0.3
                input.shift = f % 90 < 10;
                earthgrain::display(state, input, frame);
                if (!opxy::draw::valid(frame)) {
                    std::printf("FAIL an invalid frame (mode %u, dust from the start %d, frame %d)\n", mode, start, f);
                    ++failures;
                    break;
                }
            }
        }

    // Spring names Shape for what it does there: the cycle, in seconds or, on tempo, beats.
    struct Case { unsigned mode, sync; opxy::i32 shape; const char* title; const char* value; };
    const Case cases[] = {{2, 0, 0, "cycle", ".25s"}, {2, 0, 0x40000000, "cycle", "2.7s"}, {2, 0, 0x7fffffff, "cycle", "30s"},
                          {2, 1, 0, "cycle", ".5b"}, {2, 1, 0x40000000, "cycle", "8b"}, {2, 1, 0x7fffffff, "cycle", "64b"}, {0, 0, 0x40000000, "shape", "50"}};
    for (const Case& c : cases) {
        earthgrain::DisplayState state;
        earthgrain::display_reset(state);
        opxy::draw::Input input{};
        input.elapsed = 34;
        input.track = 14;
        for (unsigned i = 0; i < 4; ++i) input.shown[i] = static_cast<opxy::u8>(i);
        input.choices[0] = static_cast<opxy::u8>(c.mode);
        input.choices[2] = static_cast<opxy::u8>(c.sync);
        input.slots[2] = c.shape;
        earthgrain::display(state, input, frame);
        bool title = false, value = false;
        for (const auto& t : frame.texts) {
            title = title || std::strcmp(t.chars, c.title) == 0;
            value = value || std::strstr(t.chars, c.value) == t.chars;
        }
        if (!title || !value) {
            std::printf("FAIL mode %u sync %u: expected %s %s; texts:", c.mode, c.sync, c.title, c.value);
            for (const auto& t : frame.texts) std::printf(" [%s]", t.chars);
            std::printf("\n");
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
