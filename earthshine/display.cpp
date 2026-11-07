#include "display.h"
#include "panel.h"

namespace earthshine {
// The Earth in space, inside a dial from the moon (all sub shimmer) to the sun
// (all up). Light turns the dial's pointer, and the sun's side of the Earth
// lights as it turns toward the sun, night covering it toward the moon; the
// Earth turns west to east with the dial, once over its whole sweep. Dry is
// the Earth's size: nearer, the more dry passes. Three meridians turn with
// it, and its axis leans 23.44 degrees through the poles. Mod sets the axis
// nodding and the Earth drifting round, wider and faster with more; at Mod 0
// an untouched screen stays still. Outlines are redrawn only when they move
// about a pixel, so the redraw stays small. The shift row's intervals sit under the icons, and Shift names
// them in the header. Every move eases in about 0.15 s; the turn coasts a
// little longer.
using opxy::i32;
using opxy::u32;
using opxy::u8;
using namespace opxy::draw;
using namespace panel;

namespace {
// The controls, as the effect's (fx.cpp), and the shown ones' places in DisplayState::shown.
enum { kDecay, kTone, kLight, kShimmer, kDry, kMod, kFine, kWidth };
enum { kShownLight, kShownShimmer, kShownDry, kShownMod };
const i32 kTurn = 1 << 20;            // Q20 turns
const i32 kStart = -kTurn / 18;       // 20 degrees east faces us first
const i32 kTilt = 234, kTip = 180;    // the axis's lean (23.44 degrees) and tip toward us, tenths
const i32 kWobble = 380;              // Mod's widest nod, tenths

i32 px(i32 pixels) { return pixels * unit; }
// A Q15 control's share of a span, in the span's units.
i32 share(i32 span, i32 q15) { return span * q15 >> 15; }
i32 q15(i32 slot) { return clamp(slot, 0, 0x7fffffff) >> 16; }

// A 33-point curve at a Q15 control, interpolated.
i32 curve(const i32 (&table)[33], i32 q) {
    const i32 at = q >> 10, part = q & 1023;
    return at >= 32 ? table[32] : table[at] + ((table[at + 1] - table[at]) * part >> 10);
}

u32 root(u32 n) {
    u32 r = 0;
    for (u32 bit = 1u << 30; bit; bit >>= 2) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

// n in decimal at out, a point before the last digit when asked, then unit_text.
void number(char* out, i32 n, const char* unit_text, bool tenths = false) {
    char d[8];
    u32 k = 0;
    do { d[k++] = static_cast<char>('0' + n % 10); n /= 10; } while (n || (tenths && k < 2));
    while (k) {
        *out++ = d[--k];
        if (tenths && k == 1) *out++ = '.';
    }
    while (*unit_text) *out++ = *unit_text++;
    *out = 0;
}

// The header's captions: Decay in seconds, Tone in hertz, Fine in cents, the rest 0..100.
void captions(char (&values)[8][header_capacity], const i32 (&slots)[8]) {
    static const u32 kAmounts[5] = {kLight, kShimmer, kDry, kMod, kWidth};
    for (u32 i : kAmounts) header_amount(values[i], header_capacity, slots[i]);
    const i32 tenths = (curve(kDecayTenths, q15(slots[kDecay])) + 512) >> 10;
    if (tenths < 100) number(values[kDecay], tenths, "s", true);
    else number(values[kDecay], (tenths + 5) / 10, "s");
    const i32 hz = curve(kToneHz, q15(slots[kTone]));
    if (hz < 1000) number(values[kTone], (hz + 5) / 10 * 10, "");
    else number(values[kTone], (hz + 50) / 100, "k", true);
    // Fine: -25..+25 cents about its middle.
    const i32 cents = (q15(slots[kFine]) - 16384) * 25;
    const i32 c = cents < 0 ? -((-cents + 8192) >> 14) : (cents + 8192) >> 14;
    char* out = values[kFine];
    if (c > 0) *out++ = '+';
    if (c < 0) *out++ = '-';
    number(out, c < 0 ? -c : c, "c");
}

// The day side as one outline, about the Earth's centre, y down: the limb's
// half toward the sun, then the terminator's half ellipse back. sz is the
// sun's nearness to us (cos of its angle from the viewpoint), Q10; u the sun's
// direction on the screen, y up, Q10. The terminator stops 0.6 px short of the
// limb on a crescent, so its horns end flat rather than turning straight
// back; a gibbous side carries on smoothly, so only keeps its points apart.
void day_side(Frame::path_type& path, i32 radius, i32 sz, i32 ux, i32 uy) {
    const i32 wx = -uy, wy = ux, inset = radius - (sz < 0 ? 614 : 160);
    Point p[kPoints];
    for (u32 k = 0; k < 32; ++k) {
        const i32 a = -900 + 1800 * static_cast<i32>(k) / 31;
        const i32 c = sine(a + 900), s = sine(a);
        const i32 x = radius * ((ux * c + wx * s) >> 10) >> 10, y = radius * ((uy * c + wy * s) >> 10) >> 10;
        p[k] = Point{x, -y};
    }
    for (u32 k = 0; k < 32; ++k) {
        // Halfway between even angles and even steps along the axis, so the
        // points stay apart however flat the ellipse.
        const i32 a = 900 - 1800 * static_cast<i32>(k) / 31;
        const i32 t = (sine(a) + 1024 - 2048 * static_cast<i32>(k) / 31) / 2;   // Q10, 1 down to -1
        const i32 c = static_cast<i32>(root(static_cast<u32>(1048576 - t * t)));  // Q10
        const i32 along = inset * t >> 10, across = radius * (sz * c >> 10) >> 10;
        const i32 x = (wx * along - ux * across) >> 10, y = (wy * along - uy * across) >> 10;
        p[32 + k] = Point{x, -y};
    }
    // Start mid-limb, where both edges are long.
    for (u32 k = 0; k < kPoints; ++k) path.points[k] = p[(k + 16) % kPoints];
}

// A meridian's great circle, the half facing us, as a thin closed ribbon about
// the Earth's centre, y down. l is where it meets the limb and f the point
// nearest us, both Q10 on the screen with y up: the half runs t * l + c * f
// from one limb point to the other, t from -1 to 1 and c = sqrt(1 - t^2).
void meridian(Frame::path_type& path, i32 r_px, i32 lx, i32 ly, i32 fx, i32 fy) {
    Point p[kPoints];
    for (u32 k = 0; k < 32; ++k) {
        // Halfway between even angles and even steps, as the terminator's.
        const i32 a = -900 + 1800 * static_cast<i32>(k) / 31;
        const i32 t = (sine(a) - 1024 + 2048 * static_cast<i32>(k) / 31) / 2;
        const i32 c = static_cast<i32>(root(static_cast<u32>(1048576 - t * t)));
        const i32 x = r_px * (t * lx + c * fx) >> 10, y = r_px * (t * ly + c * fy) >> 10;
        // The ribbon's other edge half a pixel across the curve.
        i32 vx = (c * lx - t * fx) >> 10, vy = (c * ly - t * fy) >> 10;
        i32 length = static_cast<i32>(root(static_cast<u32>(vx * vx + vy * vy)));
        if (length < 64) {
            vx = lx;
            vy = ly;
            length = static_cast<i32>(root(static_cast<u32>(lx * lx + ly * ly)));
        }
        const i32 nx = -vy * 512 / length, ny = vx * 512 / length;
        p[k] = Point{x, -y};
        p[63 - k] = Point{x + nx, -(y + ny)};
    }
    // Start midway along, where both edges are long.
    for (u32 k = 0; k < kPoints; ++k) path.points[k] = p[(k + 16) % kPoints];
}

// Q10 of a step to whole multiples of a notch, rounding to the nearest.
i32 notched(i32 value, i32 notch) {
    return (value + (value < 0 ? -notch / 2 : notch / 2)) / notch * notch;
}
}

void display_reset(DisplayState& state) {
    state.ready = 0;
    state.header.reset();
}

void display(DisplayState& state, const Input& input, Frame& frame) {
    const bool still = input.reduced_motion != 0;
    const i32 dt = clamp(input.elapsed, 0, 102);  // Q10 seconds, late frames clamped

    // The shown controls, eased: a turn settles within about 0.15 s.
    static const u32 kSlots[4] = {kLight, kShimmer, kDry, kMod};
    const i32 rate = still || !state.ready ? unit : clamp(dt * 24, 0, unit);
    for (u32 i = 0; i < 4; ++i) {
        const i32 target = q15(input.slots[kSlots[i]]);
        const i32 step = (target - state.shown[i]) * rate >> 10;
        state.shown[i] = step ? state.shown[i] + step : target;
    }
    const i32 light = state.shown[kShownLight];
    const i32 dry = state.shown[kShownDry], mod = state.shown[kShownMod];

    // The turn: Light's whole sweep turns the Earth once west to east, a new
    // interval a further eighth; Mod drifts it round, 0.12 turns a second at full.
    const i32 light_now = q15(input.slots[kLight]);
    const u32 choices = static_cast<u32>(input.choices[0]) | static_cast<u32>(input.choices[1]) << 8 |
                        static_cast<u32>(input.choices[2]) << 16 | static_cast<u32>(input.choices[3]) << 24;
    if (!state.ready) {
        state.spin = state.target = kStart;
        state.last_light = light_now;
        state.last_choices = choices;
        state.wobble = 0;
    }
    state.target += (light_now - state.last_light) * (kTurn >> 15);
    if (choices != state.last_choices) state.target += kTurn / 8;
    state.last_light = light_now;
    state.last_choices = choices;
    if (!still && mod > 0) {
        state.target += (share(125829, mod) * dt) >> 10;  // 0.12 turns a second, Q20
        const u32 hz = static_cast<u32>(123 + share(563, mod));  // Q10: 0.12..0.67 Hz
        state.wobble += hz * static_cast<u32>(dt) << 12;
    }
    const i32 apart = state.target - state.spin;
    const i32 coast = still ? unit : clamp(dt * 5, 0, unit);  // about 0.2 s
    const i32 step = (apart >> 10) * coast + ((apart & 1023) * coast >> 10);
    state.spin = step ? state.spin + step : state.target;
    if (state.spin > 64 * kTurn || state.spin < -64 * kTurn) {  // keep both in range, whole turns apart
        const i32 whole = state.spin & ~(kTurn - 1);
        state.spin -= whole;
        state.target -= whole;
    }
    state.ready = 1;

    // The header: page 1 over page 2; while Shift is held the edited row names
    // the Shift row instead: the intervals, Delay and Swell.
    static const u8 kPages[2][4] = {{kDecay, kTone, kLight, kShimmer}, {kDry, kMod, kFine, kWidth}};
    static const char* const kTitles[3][8] = {
        {"decay", "tone", "light", "shimmer", "dry", "mod", "fine", "width"},
        {"delay", "swell", "sub", "up", "dry", "mod", "fine", "width"},
        {"decay", "tone", "light", "shimmer", "delay", "swell", "sub", "up"}};
    // In seconds, as Decay's caption, so each fits its header box.
    static const char* const kDelays[4] = {"0s", ".1s", ".25s", ".5s"};
    static const char* const kSwells[4] = {"off", ".5s", "1s", "2s"};
    // The Shift row's choices: Delay, Swell, Sub, Up.
    const u32 delay = input.choices[0] < 4 ? input.choices[0] : 0, swell = input.choices[1] < 4 ? input.choices[1] : 0;
    const u32 sub = input.choices[2] < 3 ? input.choices[2] : 0, up = input.choices[3] < 3 ? input.choices[3] : 0;
    char values[8][header_capacity];
    captions(values, input.slots);
    u32 page = 0;
    for (u32 col = 0; col < 3; ++col)
        if (input.shown[col] == kPages[1][col]) page = 1;
    if (input.shift) {
        const u8* row = kPages[page];
        copy(values[row[0]], header_capacity, kDelays[delay]);
        copy(values[row[1]], header_capacity, kSwells[swell]);
        copy(values[row[2]], header_capacity, kSub[sub]);
        copy(values[row[3]], header_capacity, kUp[up]);
    }
    page_header(state.header, input, frame, kTitles[input.shift ? 1 + page : 0], values, kNodeBars, kPages);

    // The intervals under the icons.
    text(frame, kTextSub, kSubX - kSubWidth[sub] / 2, kLabelY, unit, kSub[sub]);
    text(frame, kTextUp, kUpX - kUpWidth[up] / 2, kLabelY, unit, kUp[up]);

    // The pointer, from the moon's end of the dial to the sun's.
    const i32 dial = kMoonTenths + share(kSunTenths - kMoonTenths, light);
    place(frame, kNodePointer, px(kCx), px(kCy), unit, kShadePointer, fill_and_stroke);
    turn(frame, kNodePointer, rotation(-dial));

    // The Earth, nearer with more dry, in whole pixels.
    const i32 r_px = (px(kFar) + share(px(kNear - kFar), dry) + 512) >> 10, radius = px(r_px);
    place(frame, kNodeDisc, px(kCx), px(kCy), radius / 32, kShadeNight, fill_and_stroke);

    // The sun: from behind the Earth at the moon's end to behind us at the sun's.
    // Its nearness moves in steps that move the terminator 2 px, so the day
    // side's outline is redrawn only when it visibly changes.
    const i32 phi = share(1800, 32767 - light);
    const i32 sun_x = sine(kSunIconTenths + 900), sun_y = sine(kSunIconTenths);
    const i32 notch = 2048 / r_px;
    const i32 raw = sine(phi + 900);
    const i32 sz = clamp(notched(raw, notch), -1024, 1024);
    // The outline always holds a shape (a frame's paths need one); the day side
    // is hidden once its crescent is thinner than 0.6 px.
    const u32 key = 1 + (static_cast<u32>(r_px) << 12) + static_cast<u32>(sz + 1024);
    if (frame.paths[kNodeDay].key != key) {
        frame.paths[kNodeDay].key = key;
        day_side(frame.paths[kNodeDay], radius, sz, sun_x, sun_y);
    }
    if (radius * (1024 + sz) >> 10 < 614) hide(frame, kNodeDay);
    else place(frame, kNodeDay, px(kCx), px(kCy), unit, kShadeDay, fill_and_stroke);

    // The axis: its lean and tip, nodding wider with Mod.
    const i32 depth = static_cast<i32>(root(static_cast<u32>(mod) << 15)) * kWobble >> 15;
    const i32 wp = static_cast<i32>(state.wobble >> 16) * 3600 >> 16;
    const i32 lean = kTilt + (depth * sine(wp) >> 10), tip = kTip + (depth * 7 / 10 * sine(wp + 900) >> 10);
    const i32 cl = sine(lean + 900), sl = sine(lean), ct = sine(tip + 900), st = sine(tip);
    // Through the poles and 6 px past the limb either side.
    const i32 reach = px(r_px + 6);
    place(frame, kNodeAxis, px(kCx), px(kCy), clamp(reach / kAxisHalf, 64, 2 * unit), kShadeAxis, fill_and_stroke);
    turn(frame, kNodeAxis, rotation(lean));

    // The meridians: great circles through the poles, a sixth of a turn apart,
    // turned west to east, tipped and leaned as the axis. Each is redrawn only
    // when its limb point or nearest point moves a pixel.
    const i32 spin = static_cast<i32>((static_cast<u32>(state.spin & (kTurn - 1)) * 3600u) >> 20);
    const i32 pixel = 1024 / r_px;  // a pixel's move, Q10 of the radius
    for (u32 m = 0; m < kMeridians; ++m) {
        // The circle's plane's normal, (cos, 0, -sin) of its longitude, turned.
        const i32 longitude = spin + 600 * static_cast<i32>(m);
        const i32 x0 = sine(longitude + 900), z0 = -sine(longitude);
        const i32 y1 = -(z0 * st) >> 10, z1 = z0 * ct >> 10;
        const i32 qx = (x0 * cl + y1 * sl) >> 10, qy = (y1 * cl - x0 * sl) >> 10, qz = z1;
        // Its limb point is square to the normal on the screen, and its nearest
        // point toward us; face on, the circle lies along the limb.
        const i32 qxy = static_cast<i32>(root(static_cast<u32>(qx * qx + qy * qy)));
        i32 lx = 1024, ly = 0;
        if (qxy >= 16) {
            lx = qy * 1024 / qxy;
            ly = -qx * 1024 / qxy;
        }
        const i32 fx = qz * ly >> 10, fy = -(qz * lx) >> 10;
        const i32 nlx = notched(lx, pixel), nly = notched(ly, pixel), nfx = notched(fx, pixel), nfy = notched(fy, pixel);
        const u32 path_key = 1 + (static_cast<u32>(r_px) << 26 ^ static_cast<u32>(nlx + 2048) << 13 ^ static_cast<u32>(nly + 2048) ^
                                  static_cast<u32>(nfx + 2048) << 19 ^ static_cast<u32>(nfy + 2048) << 6);
        Frame::path_type& path = frame.paths[kNodeMeridians + m];
        if (path.key != path_key) {
            path.key = path_key ? path_key : 1;
            meridian(path, r_px, nlx, nly, nfx, nfy);
        }
        place(frame, kNodeMeridians + m, px(kCx), px(kCy), unit, kShadeMeridian, stroke);
    }
}
}
