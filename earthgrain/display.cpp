#include "display.h"
#include "panel.h"

namespace earthgrain {
// Earthshine's globe among fixed stars, drawn for each mode as it plays (the screens'
// names, then the effect's modes):
//   Weather (Cloud):     each grain is a pressure system; the isobars are their contours,
//                        flashing at a system's heart while its grain sounds.
//   Satellite (Stretch): the Earth's turn is the recording's time; four satellites, one
//                        for each grain voice, fly over it at what the stretch is.
//   Orbit (Spring):      a moon on an orbit. Its distance is the gap between grains; each
//                        grain leaves a dot on the orbit.
//   Plates (Slice):      the globe cut into plates; each slice lifts one out.
// A grain is drawn the same way wherever it is a point: a dot that grows and brightens with
// its envelope. The lines are short dashes (456 nodes), the dots 24 discs, and four outlines
// are reshaped each frame. The designs are mockups/fragmented-spring.html's.
//
// The screen sees only the controls, not the audio: the grains it shows are
// imagined from the controls with each mode's own timing, not the effect's own.
using opxy::i32;
using opxy::i16;
using opxy::u32;
using opxy::u16;
using opxy::u8;
using namespace opxy::draw;
using namespace panel;

namespace {
// The controls and Shift row choices, as the effect's (fx.cpp).
enum { kActivity, kSize, kShape, kPitch, kDrift, kRepeats, kDroop, kSpace };
enum { kChoiceMode, kChoiceMotion, kChoiceSync, kChoiceHold };
const i32 kOne = 32768;              // Q15
const i32 kRadiansTenths = 573;      // tenths of a degree in a radian

i32 px(i32 pixels) { return pixels * unit; }
i32 q15(i32 slot) { return clamp(slot, 0, 0x7fffffff) >> 16; }
// a times a Q15 factor b (|b| <= 1, or a little more), in 32 bits: the firmware has no 64-bit helpers.
i32 mul(i32 a, i32 b) { return (a >> 15) * b + ((a & 32767) * b >> 15); }
i32 smooth(i32 x) {  // x^2 (3 - 2x), Q15
    x = clamp(x, 0, kOne);
    return (x * x >> 15) * (3 * kOne - 2 * x) >> 15;
}
// A 33-point table at a Q15 position, interpolated.
i32 curve(const u16 (&table)[33], i32 q) {
    q = clamp(q, 0, kOne - 1);
    const i32 at = q >> 10, part = q & 1023;
    return table[at] + ((table[at + 1] - table[at]) * part >> 10);
}
// e^-x for x in Q10, Q15.
i32 decay(i32 x) {
    if (x <= 0) return kOne - 1;
    if (x >= 8 * unit) return 0;
    const i32 at = x >> 8, part = x & 255;
    return kDecay[at] + ((kDecay[at + 1] - kDecay[at]) * part >> 8);
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
u32 next(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
// The palette grey nearest a level, 0..255.
u32 grey(i32 level) { return kGreyOf[clamp(level, 0, 255)]; }

// The globe's turn and lean: a point on the unit sphere (x right, y up, z toward
// us, Q10) as it is seen, turned by `spin` (tenths) about the axis.
struct View {
    i32 cs, ss, ct, st, cl, sl;
    void at(i32 spin) {
        cs = sine(spin + 900); ss = sine(spin);
        ct = sine(kTipTenths + 900); st = sine(kTipTenths);
        cl = sine(kLeanTenths + 900); sl = sine(kLeanTenths);
    }
    void see(i32 x, i32 y, i32 z, i32& sx, i32& sy, i32& sz) const {
        const i32 xs = (x * cs + z * ss) >> 10, zs = (-x * ss + z * cs) >> 10;
        const i32 y1 = (y * ct - zs * st) >> 10, z1 = (y * st + zs * ct) >> 10;
        sx = (xs * cl + y1 * sl) >> 10;
        sy = (-xs * sl + y1 * cl) >> 10;
        sz = z1;
    }
};

// The day side as one outline, about the centre, y down (Earthshine's): the limb's
// half toward the sun, then the terminator's half ellipse back.
void day_side(Frame::path_type& path, i32 radius, i32 sz, i32 ux, i32 uy) {
    const i32 wx = -uy, wy = ux, inset = radius - (sz < 0 ? 614 : 160);
    Point p[kPoints];
    for (u32 k = 0; k < 32; ++k) {
        const i32 a = -900 + 1800 * static_cast<i32>(k) / 31;
        const i32 c = sine(a + 900), s = sine(a);
        p[k] = Point{radius * ((ux * c + wx * s) >> 10) >> 10, -(radius * ((uy * c + wy * s) >> 10) >> 10)};
    }
    for (u32 k = 0; k < 32; ++k) {
        const i32 a = 900 - 1800 * static_cast<i32>(k) / 31;
        const i32 t = (sine(a) + 1024 - 2048 * static_cast<i32>(k) / 31) / 2;
        const i32 c = static_cast<i32>(root(static_cast<u32>(1048576 - t * t)));
        const i32 along = inset * t >> 10, across = radius * (sz * c >> 10) >> 10;
        p[32 + k] = Point{(wx * along - ux * across) >> 10, -((wy * along - uy * across) >> 10)};
    }
    for (u32 k = 0; k < kPoints; ++k) path.points[k] = p[(k + 16) % kPoints];
}

// A meridian's facing half as a thin closed ribbon (Earthshine's): l where it
// meets the limb and f its point nearest us, Q10 on the screen with y up.
void meridian(Frame::path_type& path, i32 r_px, i32 lx, i32 ly, i32 fx, i32 fy) {
    Point p[kPoints];
    for (u32 k = 0; k < 32; ++k) {
        const i32 a = -900 + 1800 * static_cast<i32>(k) / 31;
        const i32 t = (sine(a) - 1024 + 2048 * static_cast<i32>(k) / 31) / 2;
        const i32 c = static_cast<i32>(root(static_cast<u32>(1048576 - t * t)));
        const i32 x = r_px * (t * lx + c * fx) >> 10, y = r_px * (t * ly + c * fy) >> 10;
        i32 vx = (c * lx - t * fx) >> 10, vy = (c * ly - t * fy) >> 10;
        i32 length = static_cast<i32>(root(static_cast<u32>(vx * vx + vy * vy)));
        if (length < 64) {
            vx = lx;
            vy = ly;
            length = static_cast<i32>(root(static_cast<u32>(lx * lx + ly * ly)));
            if (length < 1) length = 1;
        }
        const i32 nx = -vy * 512 / length, ny = vx * 512 / length;
        p[k] = Point{x, -y};
        p[63 - k] = Point{x + nx, -(y + ny)};
    }
    for (u32 k = 0; k < kPoints; ++k) path.points[k] = p[(k + 16) % kPoints];
}

i32 notched(i32 value, i32 notch) { return (value + (value < 0 ? -notch / 2 : notch / 2)) / notch * notch; }

// A meridian's great circle through the poles at a longitude (tenths): its point at
// angle t is cos t * a + sin t * b, both as seen (Q10, y up, z toward us).
struct Circle { i32 ax, ay, az, bx, by, bz; };
Circle circle(const View& view, i32 longitude) {
    Circle c;
    view.see(sine(longitude + 900), 0, sine(longitude), c.ax, c.ay, c.az);
    view.see(0, unit, 0, c.bx, c.by, c.bz);
    return c;
}

// Pitch: each set's speeds as octaves (Q10), as the effect's.
const i16 kOctaves[8][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, -1024, 0, -1024}, {0, 1024, 0, 1024}, {0, 599, -1024, 599},
                            {-1024, 0, 1024, 0}, {0, 0, 0, 0}, {0, 1024, 1623, 2048}};

// n in decimal at out, a point before the last digit when asked, then a unit.
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

void captions(char (&values)[8][header_capacity], const i32 (&slots)[8]) {
    static const u32 kAmounts[6] = {kActivity, kShape, kDrift, kRepeats, kDroop, kSpace};
    for (u32 i : kAmounts) header_amount(values[i], header_capacity, slots[i]);
    // Size in at most four characters: 20ms..99ms, then .10s...99s, then 1.0s..1.2s.
    const i32 ms = curve(kSizeMs, q15(slots[kSize]));
    if (ms < 100) {
        number(values[kSize], ms, "ms");
    } else if (ms < 995) {
        char* out = values[kSize];
        *out++ = '.';
        number(out, (ms + 5) / 10, "s");
    } else {
        number(values[kSize], (ms + 50) / 100, "s", true);
    }
    static const char* const kPitches[8] = {"norm", "uni", "oct-", "oct+", "5th", "8ves", "rev", "harm"};
    copy(values[kPitch], header_capacity, kPitches[clamp(q15(slots[kPitch]) * 8 >> 15, 0, 7)]);
}

// sin of an angle in tenths of a degree, Q10, from a table: multiplies and shifts
// only (the SDK's sine() divides, which costs too much for every speck).
inline i32 fast_sine(i32 tenths) { return kSine[(tenths * 18641 >> 16) & 1023]; }
inline i32 fast_cosine(i32 tenths) { return kSine[((tenths * 18641 >> 16) + 256) & 1023]; }

// ---- Shared ----
inline i32 fs(i32 tenths) { return fast_sine(tenths); }
inline i32 fc(i32 tenths) { return fast_cosine(tenths); }
// An angle turning at `rate` tenths a second, `now` (Q10 s) into the screen: tenths,
// 0..3599 (32-bit throughout, for days).
i32 angle_of(u32 now, i32 rate) {
    const i32 whole = static_cast<i32>(((now >> 10) % 3600) * static_cast<u32>(rate) % 3600);
    return (whole + (static_cast<i32>(now & 1023) * rate >> 10)) % 3600;
}
inline i32 absolute(i32 v) { return v < 0 ? -v : v; }
// The length of (dx, dy), Q10 px (to within 1/64 px).
i32 length(i32 dx, i32 dy) {
    const u32 x = static_cast<u32>(absolute(dx)) >> 4, y = static_cast<u32>(absolute(dy)) >> 4;
    return static_cast<i32>(root(x * x + y * y)) << 4;
}
// The direction of (dx, dy) as a unit turn, Q10, exact to rounding at any length, and
// its length (Q10 px), from one root: the vector is brought to 4096..16383 by halving or
// doubling, and the root's length scaled back.
Turn direction(i32 dx, i32 dy, i32& len) {
    i32 up = 0;
    while (absolute(dx) > 16383 || absolute(dy) > 16383) { dx >>= 1; dy >>= 1; ++up; }
    while ((dx || dy) && absolute(dx) < 4096 && absolute(dy) < 4096) { dx <<= 1; dy <<= 1; --up; }
    const i32 r = static_cast<i32>(root(static_cast<u32>(dx * dx + dy * dy)));
    if (r < 1) { len = 0; return Turn{unit, 0}; }
    len = up >= 0 ? r << up : r >> -up;
    const i32 inverse = (1 << 26) / r;
    return Turn{dx * inverse >> 16, dy * inverse >> 16};
}
// A grain's envelope at x (Q15 through it), its attack and release set by Shape as the
// effect's: attack 1% to 91% (Shape^2), release 10% + 60% x (1 - Shape).
i32 envelope(i32 x, i32 shape) {
    if (x <= 0 || x >= kOne) return 0;
    const i32 attack = 328 + mul(29819, mul(shape, shape));
    i32 release = 3277 + mul(19661, kOne - shape);
    if (release > kOne - attack) release = kOne - attack;
    if (x < attack) return smooth((x << 15) / attack);
    if (x > kOne - release) return smooth(((kOne - x) << 15) / (release > 0 ? release : 1));
    return kOne;
}

// A direction on the unit sphere: x right, y up, z toward us, Q10.
struct Dir { i32 x, y, z; };
Dir toward(i32 lat, i32 lon) {
    const i32 c = fc(lat);
    return Dir{c * fs(lon) >> 10, fs(lat), c * fc(lon) >> 10};
}
// The globe as a screen draws it: its centre (Q10 px), radius (px) and lean (Q10).
struct Globe {
    i32 x, y, r, cl, sl;
    Point at(const Dir& d) const { return Point{x + r * ((d.x * cl + d.y * sl) >> 10), y + r * ((d.x * sl - d.y * cl) >> 10)}; }
    // A direction round the back, pulled to the limb: so a region shows only its front.
    Point seen(Dir d) const {
        if (d.z < 0) {
            const i32 h = static_cast<i32>(root(static_cast<u32>(d.x * d.x + d.y * d.y)));
            if (h > 0) { d.x = d.x * 1024 / h; d.y = d.y * 1024 / h; }
        }
        return at(d);
    }
    // Whether the sun lights it (the day side, as the panel's).
    bool lit(const Dir& d) const {
        const i32 xs = (d.x * cl + d.y * sl) >> 10, ys = (d.y * cl - d.x * sl) >> 10;
        return xs * kSunX + ys * kSunY + d.z * kSunNear > 0;
    }
};
// The globe's lean: 23.44 degrees, nodding with Drift up to 38 either way (Earthshine's,
// with Mod), 0.12 to 0.67 times a second.
Globe globe(const DisplayState& state, i32 cx, i32 cy, i32 r, i32 drift) {
    const i32 depth = 380 * static_cast<i32>(root(static_cast<u32>(drift) << 15)) >> 15;
    const i32 lean = kLeanTenths + (depth * fs(state.wobble >> 10) >> 10);
    return Globe{px(cx), px(cy), r, fc(lean), fs(lean)};
}

// The dashes: lines are drawn as runs of them, each at most 6 px (the artwork's 3 px, scaled).
struct Ink {
    Frame& frame;
    u32 used;
    u32 dots;
};
void dash_line(Ink& ink, Point a, Point b, i32 level) {
    const i32 dx = b.x - a.x, dy = b.y - a.y;
    i32 len;
    const Turn t = direction(dx, dy, len);
    if (len < 96) return;
    const i32 n = (len + 6143) / 6144;
    const i32 scale = clamp(n == 1 ? len * 341 >> 10 : len / (3 * n), 64, 2 * unit);
    for (i32 k = 1; k <= n; ++k) {
        if (ink.used >= kSpecks) return;
        const u32 node = kNodeSpecks + ink.used++;
        place(ink.frame, node, a.x + dx * k / n, a.y + dy * k / n, scale, grey(level), fill_and_stroke);
        turn(ink.frame, node, t);
    }
}
// A grain as the screens draw it: a filled dot, `radius` Q10 px.
void grain_dot(Ink& ink, Point p, i32 radius, i32 level) {
    if (ink.dots >= kDots) return;
    place(ink.frame, kNodeDots + ink.dots++, p.x, p.y, radius / 2, grey(level), fill_and_stroke);  // (the artwork's disc is 2 px)
}
// A grain's dot at its envelope e (Q15): 1.3 px and grey 5/15 at rest, up to 2.5 px and white.
void flash_dot(Ink& ink, Point p, i32 e) { grain_dot(ink, p, 1331 + mul(1229, e), 85 + mul(170, e)); }
void finish(Ink& ink) {
    for (u32 i = ink.used; i < kSpecks; ++i) hide(ink.frame, kNodeSpecks + i);
    for (u32 i = ink.dots; i < kDots; ++i) hide(ink.frame, kNodeDots + i);
}

u32 next_key(DisplayState& state) {
    state.key = (state.key + 1) & 0x3fffffff;
    return 0x40000000u | state.key;
}
// A filled circle of radius r (Q10 px) about the node's origin.
void disc_path(Frame::path_type& path, i32 r) {
    for (u32 k = 0; k < kPoints; ++k) {
        const i32 a = static_cast<i32>(k) * 3600 / static_cast<i32>(kPoints);
        path.points[k] = Point{fc(a) * (r >> 10), fs(a) * (r >> 10)};
    }
}
// The globe itself: the night disc with its limb and the day side.
void draw_globe(DisplayState& state, Frame& frame, i32 cx, i32 cy, i32 r) {
    if (state.day_radius != r) {
        state.day_radius = r;
        frame.paths[kNodeDay].key = r == kRadius ? 1u : 2u;
        day_side(frame.paths[kNodeDay], px(r), kSunNear, kSunX, kSunY);
    }
    place(frame, kNodeDisc, px(cx), px(cy), px(r) / 32, kShadeNight, fill_and_stroke);
    place(frame, kNodeDay, px(cx), px(cy), unit, grey(kDayGrey), fill_and_stroke);
    hide(frame, kNodeAxis);
}
void hide_path(Frame& frame, u32 node) { hide(frame, node); }

// The globe's three meridians, their facing halves as thin ribbons (Earthshine's), the
// globe turned by `spin` (tenths).
void draw_meridians(Frame& frame, i32 spin) {
    View view;
    view.at(spin);
    const i32 pixel = 1024 / kRadius;
    for (u32 m = 0; m < kMeridians; ++m) {
        const Circle c = circle(view, 600 * static_cast<i32>(m));
        i32 len = static_cast<i32>(root(static_cast<u32>(c.az * c.az + c.bz * c.bz)));
        if (len < 1) len = 1;
        const i32 lx = (c.bz * c.ax - c.az * c.bx) / len, ly = (c.bz * c.ay - c.az * c.by) / len;
        const i32 fx = (c.az * c.ax + c.bz * c.bx) / len, fy = (c.az * c.ay + c.bz * c.by) / len;
        const i32 nlx = notched(lx, pixel), nly = notched(ly, pixel), nfx = notched(fx, pixel), nfy = notched(fy, pixel);
        const u32 key = 0x20000000u | (1 + (static_cast<u32>(nlx + 2048) << 13 ^ static_cast<u32>(nly + 2048) ^
                                            static_cast<u32>(nfx + 2048) << 19 ^ static_cast<u32>(nfy + 2048) << 6));
        Frame::path_type& path = frame.paths[kNodeMeridians + m];
        if (path.key != key) {
            path.key = key;
            meridian(path, kRadius, nlx, nly, nfx, nfy);
        }
        place(frame, kNodeMeridians + m, px(kCx), px(kCy), unit, grey(kMeridianGrey), fill_and_stroke);
    }
}

// Motion's shape of a cycle at `phase` (Q15): out and back, only out, only in; eased.
i32 stretch_at(u32 motion, i32 phase) {
    const u32 shape_of = motion >> 1;
    const i32 out = shape_of == 1 ? phase : shape_of == 2 ? kOne - 1 - phase : phase < kOne / 2 ? phase * 2 : (kOne - phase) * 2;
    return smooth(clamp(out, 0, kOne));
}
// 2^f for f in Q10 octaves (0..1023): Q10, 1024..2048.
i32 two_to(i32 f) { return 1024 + (f * (672 + (f * 352 >> 10)) >> 10); }
// A grain's length, Q10 s.
i32 grain_length(i32 size) { return curve(kSizeMs, size) * 1024 / 1000; }

// ---- Orbit (the effect's Spring): a moon on an orbit ----
// The moon's distance from the globe is the gap between grains: flung out as the cycle
// expands, pulled back in as it contracts, sweeping round at 1/r^2 as an orbit does, one turn
// a cycle (expand and contract run back along the route at the start of each). Droop tips
// the far side down or up; Drift makes the moon wander. Each grain leaves a dot on the orbit
// where it began, glowing with its envelope while it sounds, then fading over a cycle. Timing
// as the effect's Spring; on tempo it assumes 120 BPM (the screen is not told the tempo).
const i32 kSpringX = 240, kSpringY = 126, kSpringR = 44;
// The cycle, Q10 s: free 0.25 s x 120^shape (to 30 s); on tempo a half beat (0.25 s at
// 120 BPM) x 2^(8 shape, whole, at most 7: 64 beats).
i32 spring_cycle(i32 shape, bool synced) {
    if (synced) return 256 << clamp(shape * 8 >> 15, 0, 7);
    const i32 octaves = shape * 7073 >> 15;  // Q10: 120 = 2^6.907
    return (256 << (octaves >> 10)) * two_to(octaves & 1023) >> 10;
}
struct Orbit {
    Globe g;
    u32 motion;
    i32 rmin, rmax;       // Q4 px
    i32 droop, drift;     // Q15
    i32 cum[65];          // how far round by each 64th of the route, Q16 of a turn
    i32 radius(i32 st) const { return rmin + ((rmax - rmin) * st >> 15); }
    // How far round at u (Q16 along the route), Q16 of a turn.
    i32 sweep(i32 u) const {
        u = clamp(u, 0, 65535);
        const i32 i = u >> 10, f = u & 1023;
        return cum[i] + ((cum[i + 1] - cum[i]) * f >> 10);
    }
    struct Spot { Point p; i32 angle; bool behind; };
    // A point on the route; Drift pushes it off: dr a share of the distance (Q15), dy Q10 px.
    Spot at(i32 u, i32 dr, i32 dy) const {
        const i32 st = stretch_at(motion, clamp(u, 0, 65535) >> 1);
        const i32 angle = sweep(u) * 3600 >> 16;
        i32 r = radius(st);
        r = r + mul(r, mul(drift, dr));
        if (r > 226 * 16) r = 226 * 16;
        const i32 bend = mul(droop, st) * (motion & 1 ? 1 : -1);  // Q15 octaves, + up
        const i32 lift = -(bend * 14 >> 5) * (r >> 4) / (rmax >> 4 ? rmax >> 4 : 1);  // Q10 px
        const i32 ry = r * 328 >> 10 < 66 * 16 ? r * 328 >> 10 : 66 * 16;
        Spot s;
        s.p = Point{g.x + (fc(angle) * r >> 4), g.y + (fs(angle) * ry >> 4) + lift + mul(drift, dy)};
        s.angle = angle;
        s.behind = fs(angle) < 0;
        return s;
    }
    bool hidden(const Spot& s) const {  // behind the globe, within its disc
        return s.behind && length(s.p.x - g.x, s.p.y - g.y) < px(g.r);
    }
};
// Where along its route the moon is shown at `phase` (Q16): breathe goes round; expand and
// contract run back along the route at the start of each cycle (at most 0.5 s, or 40% of a
// short cycle), then out again.
i32 route_at(u32 motion, i32 phase, i32 cycle) {
    if ((motion >> 1) == 0) return phase;
    i32 f = 33554432 / (cycle > 0 ? cycle : 1);  // 0.5 s of the cycle, Q16
    if (f > 26214) f = 26214;
    if (phase < f) return 65535 - (smooth(phase * 32768 / f) << 1);
    return static_cast<i32>(static_cast<u32>(phase - f) * 32768u / static_cast<u32>((65536 - f) >> 1));
}
// Spring's own pluck (Shape 0.4): a 16% attack, a 46% release.
i32 spring_envelope(i32 x) {
    if (x <= 0 || x >= kOne) return 0;
    if (x < 5112) return smooth(x * 32768 / 5112);
    if (x > 17695) return smooth((kOne - x) * 32768 / 15073);
    return kOne;
}

void spring_screen(DisplayState& state, const Input& input, Frame& frame, i32 dt, bool still) {
    const i32 activity = q15(input.slots[kActivity]), size = q15(input.slots[kSize]);
    const i32 shape = q15(input.slots[kShape]), drift = q15(input.slots[kDrift]);
    const i32 droop = q15(input.slots[kDroop]);
    const u32 motion = input.choices[kChoiceMotion] < 6 ? input.choices[kChoiceMotion] : 0;
    const bool synced = (input.choices[kChoiceSync] & 1) != 0;
    const u32 now = state.now;
    const i32 cycle = spring_cycle(shape, synced);
    const u32 before = state.spring_phase;
    if (!still) state.spring_phase += static_cast<u32>((dt << 20) / cycle) << 12;
    if (synced && state.spring_phase < before) state.spring_wait = 0;  // a grain on the beat
    const i32 stretch = stretch_at(motion, static_cast<i32>(state.spring_phase >> 17));

    // The gap, Q16 seconds: fastest x 2^(open x stretch), as the effect's.
    i32 gap;
    if (synced) {
        static const i32 kNotes[7] = {2, 3, 4, 6, 8, 12, 16};
        static const i32 kNoteOctaves[7] = {1024, 1623, 2048, 2647, 3072, 3671, 4096};  // log2, Q10
        const i32 k = clamp(activity * 7 >> 15, 0, 6);
        const i32 note = 32768 / kNotes[k];  // a note value at 120 BPM
        const i32 open = clamp((clamp(shape * 8 >> 15, 0, 7) << 10) + kNoteOctaves[k] - 3072, 0, 4096);
        const i32 e = open * stretch >> 15;
        gap = (note << (e >> 10)) * two_to(e & 1023) >> 10;
        gap = ((gap + note / 2) / note) * note;
        if (gap < note) gap = note;
    } else {
        const i32 slack = (kOne - activity) * 3 >> 5;  // Q10 octaves
        const i32 quarter = 11702 + (shape * 7073 >> 15);  // a quarter of the cycle, octaves of a sample
        const i32 widest = quarter < 15043 ? quarter : 15043;
        const i32 open = widest - (8667 + slack) > 0 ? widest - (8667 + slack) : 0;
        const i32 e = 16384 - 7132 + slack + (open * stretch >> 15);  // Q10 octaves of a Q16 second
        gap = (1 << (e >> 10)) * two_to(e & 1023) >> 10;
    }
    if (!still) {
        state.spring_wait -= dt << 6;
        if (state.spring_wait > gap + gap / 4) state.spring_wait = gap + gap / 4;
        for (u32 guard = 0; state.spring_wait <= 0 && guard < 8; ++guard) {
            Mark& m = state.marks[state.marks_next];
            state.marks_next = (state.marks_next + 1) % 48;
            m.born = now;
            m.length = static_cast<u16>(clamp(grain_length(size), 1, 65535));
            m.phase = static_cast<u16>(state.spring_phase >> 16);
            const i32 wobble = synced ? 0 : ((static_cast<i32>(next(state.random) & 65535) - 32768) * drift >> 15) / 4;
            state.spring_wait += gap + mul(gap, wobble);
        }
    }

    // The orbit: as far out as Shape's spring is large; as close in as Activity allows.
    Orbit o;
    o.g = globe(state, kSpringX, kSpringY, kSpringR, drift);
    o.motion = motion;
    o.droop = droop;
    o.drift = drift;
    o.rmax = (kSpringR + 60 + (120 * shape >> 15)) * 16;
    o.rmin = (kSpringR + 10) * 16 + mul(mul(26 * 16, kOne - activity), 13107 + mul(19661, shape));
    if (o.rmax < o.rmin + 96) o.rmax = o.rmin + 96;
    {
        i32 w[65], sum = 0;
        for (i32 i = 0; i <= 64; ++i) {
            const i32 r = o.radius(stretch_at(motion, i * 512)) >> 4;
            w[i] = (1 << 24) / (r * r);
        }
        i32 raw[65];
        raw[0] = 0;
        for (i32 i = 1; i <= 64; ++i) raw[i] = raw[i - 1] + (w[i - 1] + w[i]) / 2;
        sum = raw[64] >> 4;
        for (i32 i = 0; i <= 64; ++i) o.cum[i] = raw[i] * 4096 / (sum > 0 ? sum : 1);
    }
    const i32 u = route_at(motion, static_cast<i32>(state.spring_phase >> 16), cycle);
    const i32 a1 = angle_of(now, 2922), a2 = angle_of(now, 7850) + 573, a3 = angle_of(now, 5329) + 1146, a4 = angle_of(now, 10256);
    const Orbit::Spot moon = o.at(u, (3932 * fs(a1) >> 10) + (2294 * fs(a2) >> 10), 7 * fs(a3) + 3 * fs(a4));
    const bool moon_shown = !(moon.behind && length(moon.p.x - o.g.x, moon.p.y - o.g.y) < px(kSpringR + 8));
    const auto under_moon = [&](Point p) { return moon_shown && length(p.x - moon.p.x, p.y - moon.p.y) < px(9); };

    draw_globe(state, frame, kSpringX, kSpringY, kSpringR);
    Ink ink{frame, 0, 0};
    // The meridians: three, turning slowly (0.4 rad a second), their facing halves.
    const i32 spin = still ? 0 : angle_of(now, 229);
    for (i32 j = 0; j < 3; ++j) {
        const i32 aj = spin + 600 * j;
        if (fc(aj) < 0) continue;
        Point prev{0, 0};
        for (i32 i = 0; i <= 10; ++i) {
            const i32 a = -900 + 180 * i;
            const i32 x = fc(a) * kSpringR * fs(aj) >> 10, y = fs(a) * kSpringR;  // Q10 px, y down
            const Point p{o.g.x + ((x * o.g.cl - y * o.g.sl) >> 10), o.g.y + ((x * o.g.sl + y * o.g.cl) >> 10)};
            if (i && !under_moon(p)) dash_line(ink, prev, p, 0x44);
            prev = p;
        }
    }
    // The orbit line, faint, its far side hidden where the globe is.
    Orbit::Spot prev = o.at(0, 0, 0);
    for (i32 i = 1; i <= 64; ++i) {
        const Orbit::Spot s = o.at(i << 10, 0, 0);
        const Point mid{(prev.p.x + s.p.x) / 2, (prev.p.y + s.p.y) / 2};
        const Orbit::Spot m{mid, s.angle, s.behind && prev.behind};
        if (!o.hidden(m) && !under_moon(mid)) dash_line(ink, prev.p, s.p, s.behind ? 0x22 : 0x33);
        prev = s;
    }
    // The dots: the newest 24, none within 6.5 px of a newer one.
    const i32 keep = cycle > unit ? cycle : unit;
    Point placed[24];
    u32 shown = 0;
    for (u32 back = 1; back <= 48 && shown < 24; ++back) {
        const Mark& m = state.marks[(state.marks_next + 48 - back) % 48];
        if (!m.length) continue;
        const i32 age = static_cast<i32>(now - m.born);
        if (age < 0) continue;
        const i32 u0 = route_at(motion, m.phase, cycle);
        const Orbit::Spot start = o.at(u0, 0, 0);
        bool crowded = false;
        for (u32 k = 0; k < shown; ++k) crowded = crowded || length(placed[k].x - start.p.x, placed[k].y - start.p.y) < 6656;
        if (crowded) continue;
        placed[shown++] = start.p;
        const i32 x = age >= m.length ? kOne : (age << 15) / m.length;
        i32 faded = (age - m.length) > 0 ? ((age - m.length) << 15) / keep : 0;
        const i32 crowd = static_cast<i32>(shown) * kOne / 24;
        if (crowd > faded) faded = crowd;
        if (x >= kOne && faded >= kOne) continue;
        if (o.hidden(start) || under_moon(start.p)) continue;
        // A dot, as every screen draws a grain: while it sounds it grows and brightens with its
        // envelope; after, it fades from mid grey over a cycle (or as newer ones crowd it out).
        if (x < kOne) flash_dot(ink, start.p, spring_envelope(x));
        else grain_dot(ink, start.p, 1331, (7 - (5 * faded >> 15) > 2 ? 7 - (5 * faded >> 15) : 2) * 17);
    }
    finish(ink);
    // The moon: a small globe lit by the same sun, fuller in front of the Earth, a crescent behind.
    hide_path(frame, kNodeMeridians);
    if (moon_shown) {
        Frame::path_type& dark = frame.paths[kNodeMeridians + 1];
        if (dark.key != 3) { dark.key = 3; disc_path(dark, px(8)); }
        place(frame, kNodeMeridians + 1, moon.p.x, moon.p.y, unit, kShadeNight, fill_and_stroke);
        Frame::path_type& light = frame.paths[kNodeMeridians + 2];
        light.key = next_key(state);
        day_side(light, px(8), fs(moon.angle) / 2, kSunX, kSunY);
        place(frame, kNodeMeridians + 2, moon.p.x, moon.p.y, unit, grey(0x88), fill_and_stroke);
    } else {
        hide_path(frame, kNodeMeridians + 1);
        hide_path(frame, kNodeMeridians + 2);
    }
}

// ---- Satellite (the effect's Stretch): the Earth's turn is time ----
// The globe turns with the play point, 120 degrees for each second of the recording: slowed
// time is a slowly turning Earth, still at Activity 0. A sight fixed 25 degrees north on the
// meridian facing us is now, the play point. Four satellites, one for each grain voice (a
// grain starts on each in turn, a quarter of a grain apart), fly over the turning ground at
// what the stretch is, (1 - Activity^2) of the ground's speed: at full Activity they hang over
// the sight; lower, they circle the globe, faster the more the sound is stretched. Each flies
// a great circle through the sight (as seen from its height, so it passes over it on the
// screen), tilted its own way, and trails the way it has come (as far as it flew over 0.4 s to
// 2.4 s, by Size). Each flashes with its voice's grain (Shape), flies as high as the pitch, and
// Droop bends its orbit south as the pitch falls (north as it rises), most on the far side.
// Drift spreads them apart; their speeds differ a little, so they drift in and out of step.
// When the play point falls 3 s behind it jumps to now, the Earth spinning forward to it over
// 0.45 s. (The screen can't see notes or holds.)
struct Vec { i32 x, y, z; };
Vec unit_vec(i32 x, i32 y, i32 z) {
    i32 len = static_cast<i32>(root(static_cast<u32>(x * x + y * y + z * z)));
    if (len < 1) len = 1;
    return Vec{x * 1024 / len, y * 1024 / len, z * 1024 / len};
}
void stretch_screen(DisplayState& state, const Input& input, Frame& frame, i32 dt, bool still) {
    const i32 activity = q15(input.slots[kActivity]), size = q15(input.slots[kSize]);
    const i32 shape = q15(input.slots[kShape]), drift = q15(input.slots[kDrift]);
    const i32 droop = q15(input.slots[kDroop]);
    const u32 motion = input.choices[kChoiceMotion] < 6 ? input.choices[kChoiceMotion] : 0;
    const u32 now = state.now;
    const i32 len = grain_length(size);
    const i32 still_share = kOne - mul(activity, activity);  // the stretch: how far the satellites outrun the ground
    if (!still) {
        state.write += dt;
        state.head += dt * mul(activity, activity) >> 15;
        const i32 least = 6 + len / 2, lag = state.write - state.head;
        if (lag > 3 * unit) {
            state.leap_from = state.shown;
            state.leap_at = now;
            state.leaping = 1;
        }
        if (lag > 3 * unit || lag < least) state.head = state.write - least;
        // The grains: one on each voice in turn, a quarter of a grain apart.
        state.sat_phase += static_cast<u32>((dt << 16) / (len > 0 ? len : 1));
        // Each satellite's run over the ground (Q4 tenths), at its own speed (6% apart).
        const i32 speed = mul(1200 * 16, still_share);  // Q4 tenths a second
        for (i32 k = 0; k < 4; ++k) {
            const i32 own = speed + (speed * (2 * k - 3) * 31 >> 11);
            state.sat_run[k] = (state.sat_run[k] + (own * dt >> 10)) % (3600 * 16);
        }
    }
    if (state.leaping) {
        const i32 k = static_cast<i32>(now - state.leap_at) * 32768 / 461;
        if (k >= kOne) state.leaping = 0;
        else state.shown = state.leap_from + mul(state.head - state.leap_from, smooth(k));
    }
    if (!state.leaping) state.shown = state.head;

    const Globe g = globe(state, kCx, kCy, kRadius, drift);
    draw_globe(state, frame, kCx, kCy, kRadius);
    // The globe turns with the play point (westward as time runs on, so the ground passes under the sight).
    draw_meridians(frame, still ? 0 : 3600 - angle_of(static_cast<u32>(state.shown + (1 << 26)), 1200));
    Ink ink{frame, 0, 0};
    // The sight: four short ticks along the meridian and the parallel, 5 to 14 degrees out
    // from the play point, with a gap where they would cross.
    const i32 north = 250;
    for (i32 side = 0; side < 4; ++side) {
        const i32 dl = side == 0 ? 1 : side == 1 ? -1 : 0, dn = side == 2 ? 1 : side == 3 ? -1 : 0;
        Point prev{0, 0};
        for (i32 i = 0; i <= 4; ++i) {
            const i32 r = 50 + 90 * i / 4;
            const Point p = g.at(toward(north + dl * r, dn * (r * 1130 >> 10)));
            if (i) dash_line(ink, prev, p, 0x96);
            prev = p;
        }
    }
    // The pitch: Droop's sway, shaped by Motion (Q15 octaves, + up).
    const i32 bend = mul(droop, stretch_at(motion, state.sway)) * (motion & 1 ? 1 : -1);
    // The trail: as far as it flew over 0.4 s to 2.4 s (Size), at most 300 degrees.
    i32 trail = mul(mul(1200, still_share), 13107 + 2 * size);
    if (trail > 3000) trail = 3000;
    const i32 height = kRadius * (1208 + (123 * bend >> 15));  // Q10 px: 1.18 to 1.30 radii
    // Where on the unit sphere passes over the sight as seen from that height.
    const i32 up = clamp(433 * 1024 / (1208 + (123 * bend >> 15)), 0, 1013);
    const Vec p0{0, up, static_cast<i32>(root(static_cast<u32>(1048576 - up * up)))};
    const Vec n0{0, p0.z, -p0.y};
    const i32 sag = 1229 * bend >> 15;  // Q10: 1.2 x the bend in octaves
    const auto seen = [&](const Vec& d) {
        const i32 sx = (d.x * g.cl + d.y * g.sl) >> 10, sy = (d.x * g.sl - d.y * g.cl) >> 10;
        return Point{g.x + (height * sx >> 10), g.y + (height * sy >> 10)};
    };
    const auto hidden = [&](const Vec& d) {
        if (d.z >= 0) return false;
        const i32 across = static_cast<i32>(root(static_cast<u32>(d.x * d.x + d.y * d.y)));  // Q10
        return (across * height >> 10) < px(kRadius);
    };
    for (i32 k = 0; k < 4; ++k) {
        const i32 tilt = (2 * k - 3) * 80;  // (k - 1.5) x 16 degrees
        const Vec d0{fc(tilt), fs(tilt) * n0.y >> 10, fs(tilt) * n0.z >> 10};
        const Vec nn{(p0.y * d0.z - p0.z * d0.y) >> 10, (p0.z * d0.x - p0.x * d0.z) >> 10, (p0.x * d0.y - p0.y * d0.x) >> 10};
        const auto at = [&](i32 ang) {
            const i32 c = fc(ang), sn = fs(ang), o = sag * (1024 - c) >> 11;
            return unit_vec((p0.x * c + d0.x * sn + nn.x * o) >> 10, (p0.y * c + d0.y * sn + nn.y * o) >> 10,
                            (p0.z * c + d0.z * sn + nn.z * o) >> 10);
        };
        const i32 lead = (2 * k - 3) * (20 + (180 * drift >> 15)) + (state.sat_run[k] >> 4);
        const u32 xk = (state.sat_phase - static_cast<u32>(k) * 16384u) & 65535u;
        const i32 e = envelope(static_cast<i32>(xk >> 1), shape);
        // Its trail, fading back.
        if (trail > 10) {
            Vec prev_d = at(lead - trail);
            Point prev = seen(prev_d);
            bool prev_hidden = hidden(prev_d);
            for (i32 i = 1; i <= 24; ++i) {
                const Vec d = at(lead - trail + trail * i / 24);
                const Point p = seen(d);
                const bool h = hidden(d);
                const i32 level = ((24 + 5 * i) * (16384 + e / 2) >> 15) * 17 / 24;  // (1 + 5 i/24) x (0.5 + 0.5 e) greys
                if (!h && !prev_hidden && level >= 17) dash_line(ink, prev, p, level);
                prev = p;
                prev_hidden = h;
            }
        }
        const Vec d = at(lead);
        if (!hidden(d)) flash_dot(ink, seen(d), e);
    }
    finish(ink);
}

// ---- Weather (the effect's Cloud) ----
// Each grain is a pressure system: across, its place in the stereo field (5 degrees either way
// at Drift 0, 80 at full); up and down, how far back in the recording it came from (4 to 65
// degrees, by Drift) and its pitch (60 degrees an octave: Droop's sway sweeps the weather north
// or south). It rises and lingers as it drifts east; Shape sets its character (plucks: sudden,
// compact, strong, gone in 0.15 s; swells: building over 0.8 s, broad, soft, lingering over a
// second), Size its reach (4 to 48 degrees). The isobars are the contours of them all over a
// faint, slowly changing background. While a grain sounds, the isobars at its system's heart
// flash with its envelope, as the dots do on the other screens. Over the night they are dim
// grey (higher ones a little brighter); over the day side's grey, light enough to stand out.
// Three meridians turn slowly beneath.
const i32 kGrid = 20;
void cloud_screen(DisplayState& state, const Input& input, Frame& frame, i32 dt, bool still) {
    const i32 activity = q15(input.slots[kActivity]), size = q15(input.slots[kSize]);
    const i32 shape = q15(input.slots[kShape]), drift = q15(input.slots[kDrift]);
    const i32 droop = q15(input.slots[kDroop]);
    const u32 motion = input.choices[kChoiceMotion] < 6 ? input.choices[kChoiceMotion] : 0;
    const u32 now = state.now;
    const Globe g = globe(state, kCx, kCy, kRadius, drift);

    // Imagined grains: Activity^2 x 60 a second.
    if (!still) {
        state.due += mul(mul(activity, activity), 61440) * dt >> 10;
        if (state.due > 4 * unit) state.due = 4 * unit;
        const i32 bend = mul(droop, stretch_at(motion, state.sway)) * (motion & 1 ? 1 : -1);
        while (state.due >= unit) {
            state.due -= unit;
            const u32 r = next(state.random);
            System& s = state.systems[state.systems_next];
            state.systems_next = (state.systems_next + 1) % 16;
            const i32 back = static_cast<i32>(r & 32767), pan = static_cast<i32>((r >> 15) & 65535) - 32768;
            s.lat = static_cast<opxy::i16>(clamp(((2 * back - kOne) * (40 + (610 * drift >> 15)) >> 15) + (bend * 600 >> 15), -850, 850));
            s.lon = static_cast<opxy::i16>(pan * (50 + (750 * drift >> 15)) >> 15);
            s.born = now;
            s.length = static_cast<u16>(clamp(grain_length(size), 1, 65535));
            const u32 r2 = next(state.random);
            s.tilt = static_cast<opxy::i16>(static_cast<i32>(r2 & 1023) * 688 / 1024 - 344);
            s.stretch = static_cast<u16>(1229 + (static_cast<i32>((r2 >> 10) & 1023) * 1331 >> 10));
        }
    }

    // Each system now: its centre (Q4 px from the globe's), strength (Q15) and reach.
    struct Live { i32 x, y, w, reach, per_reach, c, s, el, per_el, e; };
    Live live[12];
    u32 count = 0;
    const i32 rise = 10 + mul(819, mul(shape, shape));               // Q10 s: 0.01 + 0.8 shape^2
    const i32 lasting = 154 + mul(1229, shape);                       // Q10 s: 0.15 + 1.2 shape
    const i32 strength = 58982 - mul(36045, shape);                   // Q15: 1.8 - 1.1 shape
    for (const System& s : state.systems) {
        if (!s.length || count >= 12) continue;
        const i32 age = static_cast<i32>(now - s.born);
        if (age < 0) continue;
        const i32 up = age < rise ? smooth((age << 15) / rise) : kOne;
        const i32 w = mul(mul(strength, up), decay(age > rise ? (age - rise) * 1024 / lasting : 0));
        if (w < 328) continue;
        const Dir d = toward(s.lat, s.lon + (age * 220 >> 10));        // drifting east, 22 degrees a second
        if (d.z < 0) continue;
        const Point p = g.at(d);
        const i32 frac = clamp(s.length * 1000 / 1024 * 32768 / 1200, 0, kOne);
        const i32 reach = mul((4 << 4) + (44 << 4) * static_cast<i32>(root(static_cast<u32>(frac) << 15)) / 32768, 22938 + mul(19661, shape));  // Q4 degrees: (4 + 44 sqrt(length / 1.2 s)) x (0.7 + 0.6 shape)
        const i32 reach_px = reach * 1100 >> 10;                       // Q4 px (63 px a radian)
        const i32 rp = reach_px > 16 ? reach_px : 16;
        const i32 e = age < s.length ? envelope((age << 15) / s.length, shape) : 0;  // its grain, sounding
        live[count++] = Live{(p.x - g.x) >> 6, (p.y - g.y) >> 6, w, rp, (1 << 20) / rp,
                             fc(s.tilt), fs(s.tilt), s.stretch, (1 << 20) / s.stretch, e};
    }

    // The field over a grid across the disc (5.25 px), Q12.
    i32* field = state.field;
    const i32 step16 = 2 * kRadius * 16 / kGrid;
    const i32 t1 = angle_of(now, 23), t2 = angle_of(now, 34), t3 = angle_of(now, 52);
    // The work is split across two frames: the field on one, the isobars from it on the next
    // (the dashes keep their places between), so neither frame carries both.
    state.cloud_turn ^= 1u;
    const bool compute = state.cloud_turn != 0;
    // The background first, everywhere (the whole globe's pressure, slowly changing).
    if (compute) {
    const i32 inside_q4 = kRadius * 16 * 63 / 64;
    for (i32 j = 0; j <= kGrid; ++j)
        for (i32 i = 0; i <= kGrid; ++i) {
            i32 x = -kRadius * 16 + i * step16, y = -kRadius * 16 + j * step16;  // Q4 px
            if (x * x + y * y > inside_q4 * inside_q4) {  // off the disc: the value just inside its edge
                const i32 rr = length(x << 6, y << 6) >> 6;
                x = x * inside_q4 / rr;
                y = y * inside_q4 / rr;
            }
            const i32 u = x * 1040 >> 10, w = -y * 1040 >> 10;  // Q10 of the radius (1/63/16), y up
            // (Faint, 40% of the 0.22 / 0.15 / 0.08 it was: the grains' systems are what matters.)
            field[j * (kGrid + 1) + i] = 2048 + ((360 * fs(((u * 802 + w * 631) >> 10) + t1) + 246 * fs(((w * 1318 - u * 917) >> 10) - t2 + 630) +
                                                 131 * fs(((u * 2120 + w * 1662) >> 10) + t3 + 1146)) >> 10);
        }
    // Then each system, over the grid points within three of its reaches (stretched).
    for (u32 k = 0; k < count; ++k) {
        const Live& s = live[k];
        const i32 span = s.reach * 7 / 2;  // (beyond e^-5 of it, nothing worth drawing)
        const i32 i0 = clamp((s.x - span + kRadius * 16) / step16, 0, kGrid), i1 = clamp((s.x + span + kRadius * 16) / step16 + 1, 0, kGrid);
        const i32 j0 = clamp((s.y - span + kRadius * 16) / step16, 0, kGrid), j1 = clamp((s.y + span + kRadius * 16) / step16 + 1, 0, kGrid);
        for (i32 j = j0; j <= j1; ++j)
            for (i32 i = i0; i <= i1; ++i) {
                const i32 dx = -kRadius * 16 + i * step16 - s.x, dy = -kRadius * 16 + j * step16 - s.y;
                const i32 a = (dx * s.c + dy * s.s) >> 10, b = (-dx * s.s + dy * s.c) >> 10;
                const i32 an = a * s.per_reach >> 10, bn = b * s.per_reach >> 10;  // Q10 reaches
                if (absolute(an) > 6000 || absolute(bn) > 6000) continue;
                const i32 e = ((an * an >> 10) * s.per_el >> 10) + ((bn * bn >> 10) * s.el >> 10);
                if (e >= 5 * unit) continue;
                field[j * (kGrid + 1) + i] += mul(s.w, decay(e)) >> 3;
            }
    }
    }
    i32 top = 0;
    for (i32 k = 0; k < (kGrid + 1) * (kGrid + 1); ++k)
        if (field[k] > top) top = field[k];

    draw_globe(state, frame, kCx, kCy, kRadius);
    // The meridians beneath, turning slowly (Earthshine's).
    draw_meridians(frame, still ? 0 : angle_of(now, 229));

    // The isobars: every 0.15 from 0.12 up through the highest (at most 8 levels, spaced
    // wider when systems pile up), by marching squares.
    if (compute) return;
    Ink ink{frame, 0, 0};
    const i32 spacing = (top - 491) / 8 > 614 ? (top - 491) / 8 : 614;
    const i32 inside = kRadius * 16;
    for (i32 level = 491; level < top && ink.used < kSpecks; level += spacing) {
        const i32 high = clamp((level - 4096) << 3, 0, kOne);  // Q15: how far above 1, up to 1
        const i32 dark = 51 + (34 * high >> 15);                // greys 3 to 5 (x 17)
        for (i32 j = 0; j < kGrid; ++j)
            for (i32 i = 0; i < kGrid; ++i) {
                const i32 f00 = field[j * (kGrid + 1) + i], f10 = field[j * (kGrid + 1) + i + 1];
                const i32 f11 = field[(j + 1) * (kGrid + 1) + i + 1], f01 = field[(j + 1) * (kGrid + 1) + i];
                const u32 c = (f00 > level ? 8u : 0u) | (f10 > level ? 4u : 0u) | (f11 > level ? 2u : 0u) | (f01 > level ? 1u : 0u);
                if (c == 0 || c == 15) continue;
                // An edge's crossing: (i0, j0) to (i1, j1), Q4 px from the centre.
                const auto cross = [&](i32 i0, i32 j0, i32 i1, i32 j1) {
                    const i32 a = field[j0 * (kGrid + 1) + i0], b = field[j1 * (kGrid + 1) + i1];
                    const i32 t = b != a ? (level - a) * 1024 / (b - a) : 512;
                    return Point{-inside + i0 * step16 + ((i1 - i0) * step16 * t >> 10), -inside + j0 * step16 + ((j1 - j0) * step16 * t >> 10)};
                };
                Point e[4];
                u32 n = 0;
                // Edges: top (i,j)-(i+1,j), right, bottom (i,j+1)-(i+1,j+1), left; each crossed when its ends differ.
                if (((c >> 3) ^ (c >> 2)) & 1) e[n++] = cross(i, j, i + 1, j);
                if (((c >> 2) ^ (c >> 1)) & 1) e[n++] = cross(i + 1, j, i + 1, j + 1);
                if (((c >> 1) ^ c) & 1) e[n++] = cross(i, j + 1, i + 1, j + 1);
                if (((c >> 3) ^ c) & 1) e[n++] = cross(i, j, i, j + 1);
                for (u32 k = 0; k + 1 < n; k += 2) {
                    const i32 mx = (e[k].x + e[k + 1].x) / 2, my = (e[k].y + e[k + 1].y) / 2;
                    const i32 q = mx * mx + my * my;
                    if (q > inside * inside) continue;  // on the globe only
                    // Lit on the day side's own edge (the terminator's half ellipse): with p toward the
                    // sun and w across it, p > -sz sqrt(1 - w^2) (sz < 0), compared squared.
                    const i32 X = mx * 1040 >> 10, Y = -my * 1040 >> 10;  // Q10 of the radius
                    const i32 side = (X * kSunX + Y * kSunY) >> 10, across = (Y * kSunX - X * kSunY) >> 10;
                    const i32 ww = 1048576 - across * across;
                    const bool lit = side > 0 && side * side > (ww >> 10) * (kSunNear * kSunNear >> 10);
                    // The flash: how near the heart of a system whose grain sounds, by its envelope.
                    i32 flash = 0;
                    for (u32 n2 = 0; n2 < count; ++n2) {
                        const Live& s = live[n2];
                        if (s.e <= 0) continue;
                        const i32 dx = mx - s.x, dy = my - s.y;
                        const i32 a = (dx * s.c + dy * s.s) >> 10, b = (-dx * s.s + dy * s.c) >> 10;
                        const i32 an = a * s.per_reach >> 10, bn = b * s.per_reach >> 10;
                        if (absolute(an) > 3000 || absolute(bn) > 3000) continue;
                        const i32 near4 = 4 * (((an * an >> 10) * s.per_el >> 10) + ((bn * bn >> 10) * s.el >> 10));  // (its pull)^4
                        const i32 f = mul(s.e, decay(near4));
                        if (f > flash) flash = f;
                    }
                    flash = flash + (flash * 3 >> 3) > kOne ? kOne : flash + (flash * 3 >> 3);  // x 1.3, at most 1
                    // Night: dim grey up to 13 as it flashes; day: grey 10 up to white.
                    const i32 level_grey = lit ? 170 + (85 * flash >> 15) : dark + ((221 - dark) * flash >> 15);
                    dash_line(ink, Point{g.x + (e[k].x << 6), g.y + (e[k].y << 6)}, Point{g.x + (e[k + 1].x << 6), g.y + (e[k + 1].y << 6)},
                              level_grey);
                }
            }
    }
    finish(ink);
}

// ---- Plates (the effect's Slice): the globe broken into plates ----
// Meridians and the equator cut the turning globe into sixteen plates, eight round the top and
// eight round the bottom. Each slice the sound plays lifts a plate facing us out of the globe
// (about 5 px at most), leaving a dark gap, its edge flashing with the slice's envelope (Shape)
// up to white, as the dots do on the other screens: the next plate on when the sound steps on
// to the next slice, any with Drift's jumps. Activity drives the screen as it drives the
// sound: the globe turns faster (6 degrees a second at 0 to 72 at full), each plate is out at
// least 0.35 s at 0 down to 0.07 s at full (so short slices are seen) and lifts a little less,
// and the next is taken after 0.18 s down to 0.025 s; faster than that, the screen shows every
// second or third. One plate at a time.
void slice_screen(DisplayState& state, const Input& input, Frame& frame, i32 dt, bool still) {
    const i32 activity = q15(input.slots[kActivity]), size = q15(input.slots[kSize]);
    const i32 shape = q15(input.slots[kShape]), drift = q15(input.slots[kDrift]);
    const u32 now = state.now;
    Globe g = globe(state, kCx, kCy, kRadius, drift);
    const i32 visible_least = 358 - mul(287, activity), pace = 184 - mul(159, activity);  // Q10 s
    if (!still) state.plate_spin = (state.plate_spin + dt * (60 + mul(660, mul(activity, activity)))) % (3600 << 10);
    const i32 spin = still ? 0 : state.plate_spin >> 10;
    const auto facing = [&](u32 piece) { return fc(static_cast<i32>(piece % 8) * 450 + 225 + spin) > 358; };

    // Imagined slices: one every 0.6 s / 16^Activity (none under 1%).
    if (!still && activity >= 328) {
        state.slice_wait -= dt;
        if (state.slice_wait > unit) state.slice_wait = unit;
        while (state.slice_wait <= 0) {
            const i32 oct = activity * 4 >> 5;  // Q10
            const i32 gap = (614 << 10) / ((1 << (oct >> 10)) * two_to(oct & 1023) >> 10 > 0 ? (1 << (oct >> 10)) * two_to(oct & 1023) >> 10 : 1);
            state.slice_wait += gap > 8 ? gap : 8;
            const u32 r = next(state.random);
            const bool jump = static_cast<i32>(r & 32767) < drift;
            state.slice = jump ? (r >> 16) % 8 : (state.slice + 1) % 8;
            // Shown only once the one shown has had 0.18 s, or has finished.
            const i32 shown_for = static_cast<i32>(now - state.slice_born);
            const i32 vis = state.slice_length > visible_least ? state.slice_length : visible_least;
            if (state.slice_shown && shown_for < pace && shown_for < vis) continue;
            u32 piece = jump ? (r >> 20) % 16 : (state.piece + 1) % 16;
            for (u32 n = 0; n < 16 && !facing(piece); ++n) piece = (piece + 1) % 16;
            state.piece = piece;
            state.slice_born = now;
            state.slice_length = grain_length(size) < 512 ? grain_length(size) : 512;
            state.slice_shown = 1;
        }
    }
    i32 e = 0;
    if (state.slice_shown) {
        const i32 vis = state.slice_length > visible_least ? state.slice_length : visible_least;
        const i32 x = static_cast<i32>(now - state.slice_born) * 32768 / vis;
        e = envelope(x, shape);
        if (x >= kOne) state.slice_shown = 0;
    }

    draw_globe(state, frame, kCx, kCy, kRadius);
    // The piece out: its outline (64 points: up one meridian, along its top, down the other,
    // back along its bottom), the gap where it was and the piece lifted out onto a slightly
    // bigger globe and pushed along its own outward direction.
    Point lifted[64];
    bool out = e >= 3277;
    if (out) {
        const i32 col = static_cast<i32>(state.piece % 8), row = static_cast<i32>(state.piece / 8);
        const i32 a = col * 450 + spin, b = a + 450, top = row ? 0 : 900, bottom = row ? -900 : 0;
        Frame::path_type& gap = frame.paths[kNodeMeridians];
        Frame::path_type& piece = frame.paths[kNodeMeridians + 1];
        gap.key = next_key(state);
        piece.key = next_key(state);
        const i32 limit = path_extent * unit;
        for (i32 k = 0; k < 64; ++k) {
            const i32 side = k / 16, s = k % 16;
            i32 lat, lon;
            if (side == 0) { lat = bottom + (top - bottom) * s / 16; lon = a; }
            else if (side == 1) { lat = top; lon = a + 450 * s / 16; }
            else if (side == 2) { lat = top - (top - bottom) * s / 16; lon = b; }
            else { lat = bottom; lon = b - 450 * s / 16; }
            const Point p = g.seen(toward(lat, lon));
            gap.points[k] = piece.points[k] = Point{clamp(p.x - g.x, -limit, limit), clamp(p.y - g.y, -limit, limit)};
        }
        const Dir mid = toward((top + bottom) / 2, a + 225);
        const i32 lifted_by = mul(e, kOne - mul(13107, activity));  // a little less at high Activity
        const i32 scale = unit + (lifted_by * 2560 / kRadius >> 15);   // onto a globe 2.5 px bigger
        const i32 push = 4 * lifted_by >> 5;                           // and 4 px out, Q10 px
        const i32 ox = ((mid.x * g.cl + mid.y * g.sl) >> 10) * push >> 10, oy = ((mid.x * g.sl - mid.y * g.cl) >> 10) * push >> 10;
        for (i32 k = 0; k < 64; ++k)
            lifted[k] = Point{g.x + ox + (piece.points[k].x * scale >> 10), g.y + oy + (piece.points[k].y * scale >> 10)};
        place(frame, kNodeMeridians, g.x, g.y, unit, kShadeNight, fill_and_stroke);
        place(frame, kNodeMeridians + 1, g.x + ox, g.y + oy, scale, e > 19661 ? kShadeEdge : e > 9830 ? kShadeEdgeMid : kShadeEdgeDim, fill_and_stroke);
    } else {
        hide_path(frame, kNodeMeridians);
        hide_path(frame, kNodeMeridians + 1);
    }
    hide_path(frame, kNodeMeridians + 2);
    // Whether a point lies on the lifted piece (its cuts are hidden by it).
    const auto covered = [&](Point p) {
        if (!out) return false;
        bool in = false;
        const i32 x = p.x >> 6, y = p.y >> 6;
        for (i32 i = 0, j = 63; i < 64; j = i++) {
            const i32 xi = lifted[i].x >> 6, yi = lifted[i].y >> 6, xj = lifted[j].x >> 6, yj = lifted[j].y >> 6;
            if ((yi > y) != (yj > y) && x < xi + (xj - xi) * (y - yi) / (yj - yi)) in = !in;
        }
        return in;
    };
    // The cuts: eight meridians and the equator, their facing halves.
    Ink ink{frame, 0, 0};
    for (i32 c = 0; c < 8; ++c) {
        Point prev{0, 0};
        bool was = false;
        for (i32 i = 0; i <= 18; ++i) {
            const Dir d = toward(-900 + 100 * i, c * 450 + spin);
            const Point p = g.at(d);
            const bool front = d.z > 0;
            if (was && front && !covered(p)) dash_line(ink, prev, p, 0x44);
            prev = p;
            was = front;
        }
    }
    {
        Point prev{0, 0};
        bool was = false;
        for (i32 i = 0; i <= 48; ++i) {
            const Dir d = toward(0, i * 75);
            const Point p = g.at(d);
            const bool front = d.z > 0;
            if (was && front && !covered(p)) dash_line(ink, prev, p, 0x44);
            prev = p;
            was = front;
        }
    }
    finish(ink);
}
}  // namespace

void display_reset(DisplayState& state) {
    state.ready = 0;
    state.header.reset();
}

void display(DisplayState& state, const Input& input, Frame& frame) {
    const bool still = input.reduced_motion != 0;
    const i32 dt = clamp(input.elapsed, 0, 102);  // Q10 seconds, late frames clamped
    const i32 drift = q15(input.slots[kDrift]);
    const u32 mode = input.choices[kChoiceMode] < 4 ? input.choices[kChoiceMode] : 0;
    if (!state.ready) {
        state.now = 0;
        state.random = 0x2545f491u;
        state.mode = 99;
        state.wobble = state.spin = state.sway = 0;
        state.key = 0;
        state.day_radius = 0;
        state.spring_phase = 0;
        state.spring_wait = 0;
        for (Mark& m : state.marks) m = Mark{0, 0, 0};
        state.marks_next = 0;
        state.due = 0;
        for (System& s : state.systems) s = System{0, 0, 0, 0, 0, 0};
        state.systems_next = 0;
        state.cloud_turn = 0;
        for (i32& v : state.field) v = 0;
        state.write = state.head = state.shown = state.leap_from = 0;
        state.leap_at = state.leaping = 0;
        state.slice_born = 0;
        state.slice_length = state.slice_wait = 0;
        state.piece = state.slice = state.slice_shown = 0;
        state.plate_spin = 0;
        state.sat_phase = 0;
        for (i32& r : state.sat_run) r = 0;
        // Every outline always holds a shape, shown or not: a frame whose paths lack one is
        // refused whole, and the screen would keep its artwork.
        for (u32 p = 0; p < 4; ++p) {
            frame.paths[p].key = 3 + p;
            disc_path(frame.paths[p], px(8));
        }
    }
    state.now += static_cast<u32>(dt);
    if (!still) {
        state.wobble = (state.wobble + dt * (432 + (1980 * drift >> 15))) % (3600 * 1024);
        state.sway = (state.sway + dt * 8) & 32767;  // a sway in 4 s
    }
    if (mode != state.mode) {  // a new screen: none of the last one's dashes left over
        state.mode = mode;
        state.day_radius = 0;
        state.cloud_turn = 0;
        for (u32 i = 0; i < kSpecks; ++i) hide(frame, kNodeSpecks + i);
        for (u32 i = 0; i < kDots; ++i) hide(frame, kNodeDots + i);
    }

    // The header: page 1 over page 2; while Shift is held the edited row names the
    // Shift row: Mode, Motion, Sync and Hold.
    static const u8 kPages[2][4] = {{kActivity, kSize, kShape, kPitch}, {kDrift, kRepeats, kDroop, kSpace}};
    static const char* const kTitles[3][8] = {
        {"activity", "size", "shape", "pitch", "drift", "repeats", "droop", "space"},
        {"mode", "motion", "sync", "hold", "drift", "repeats", "droop", "space"},
        {"activity", "size", "shape", "pitch", "mode", "motion", "sync", "hold"}};
    // The screens' names (the effect's Cloud, Stretch, Spring and Slice).
    static const char* const kModes[4] = {"wthr", "sat", "orbit", "plate"};  // (5 letters fit)
    // Motion's shape, then - for bending down or + for up.
    static const char* const kMotions[6] = {"both-", "both+", "out-", "out+", "in-", "in+"};
    static const char* const kSyncs[2] = {"free", "beat"};
    static const char* const kHolds[9] = {"inf", "64s", "32s", "16s", "8s", "4s", "2s", "1s", "off"};
    char values[8][header_capacity];
    captions(values, input.slots);
    u32 page = 0;
    for (u32 col = 0; col < 4; ++col)
        if (input.shown[col] == kPages[1][col]) page = 1;
    // In Orbit (Spring), Shape sets how long a cycle takes: named for that, in seconds (0.25 s x
    // 120^shape, to 30 s, as the effect's) or, on tempo, beats (a half to 64).
    const bool cycle = mode == 2;
    if (cycle) {
        const i32 shape = q15(input.slots[kShape]);
        if (input.choices[kChoiceSync] & 1) {
            const u32 halves = 1u << clamp(shape * 8 >> 15, 0, 7);
            if (halves == 1) copy(values[kShape], header_capacity, ".5b");
            else number(values[kShape], static_cast<i32>(halves / 2), "b");
        } else {
            const i32 octaves = shape * 7073 >> 5;  // Q20: 120 = 2^6.907
            const i32 f = octaves & 1048575;
            const i32 two_f = 32768 + ((f >> 5) * (21512 + ((f >> 5) * 11256 >> 15)) >> 15);  // 2^f, Q15
            const i32 ms = (250 << (octaves >> 20)) * (two_f >> 5) >> 10;
            if (ms < 995) {
                char* out = values[kShape];
                *out++ = '.';
                number(out, (ms + 5) / 10, "s");
            } else if (ms < 9950) {
                number(values[kShape], (ms + 50) / 100, "s", true);
            } else {
                number(values[kShape], (ms + 500) / 1000, "s");
            }
        }
    }
    const char* titles[8];
    for (u32 i = 0; i < 8; ++i) titles[i] = kTitles[input.shift ? 1 + page : 0][i];
    if (cycle && !(input.shift && page == 0)) titles[kShape] = "cycle";
    if (input.shift) {
        const u8* row = kPages[page];
        copy(values[row[0]], header_capacity, kModes[mode]);
        copy(values[row[1]], header_capacity, kMotions[input.choices[kChoiceMotion] < 6 ? input.choices[kChoiceMotion] : 0]);
        copy(values[row[2]], header_capacity, kSyncs[input.choices[kChoiceSync] & 1]);
        copy(values[row[3]], header_capacity, kHolds[input.choices[kChoiceHold] < 9 ? input.choices[kChoiceHold] : 0]);
    }
    page_header(state.header, input, frame, titles, values, kNodeBars, kPages);

    if (mode == 0) cloud_screen(state, input, frame, dt, still);
    else if (mode == 1) stretch_screen(state, input, frame, dt, still);
    else if (mode == 2) spring_screen(state, input, frame, dt, still);
    else slice_screen(state, input, frame, dt, still);
    state.ready = 1;
}
}
