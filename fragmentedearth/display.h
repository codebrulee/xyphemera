#pragma once
#include <opxy/page_header.hpp>

namespace fragmentedearth {
// Matches resources.display in definition.json: paths, points, nodes, texts,
// palette. tools/make_panel.py writes both and names every node (panel.h).
using Frame = opxy::draw::Frame<4, 64, 494, 16, 16>;

// A grain on Spring's orbit: when it began (Q10 seconds since the screen began), how
// long it lasts (Q10 seconds; 0: unused) and where in the cycle it began (Q16).
struct Mark {
    opxy::u32 born;
    opxy::u16 length;
    opxy::u16 phase;
};

// A pressure system on Cloud's map, from one imagined grain: where it is on the
// globe (tenths of a degree), when it began and how long its grain is (Q10 s), and
// its stretch and tilt.
struct System {
    opxy::i16 lat, lon;
    opxy::u32 born;
    opxy::u16 length;
    opxy::i16 tilt;   // tenths
    opxy::u16 stretch;  // Q10, 1.2 to 2.5
};

// Everything the screen keeps between frames. Times are Q10 seconds since the
// screen began; angles tenths of a degree (Q10 where named).
struct DisplayState {
    opxy::draw::PageHeader header;
    opxy::u32 ready;
    opxy::u32 now;
    opxy::u32 random;     // xorshift, for the imagined grains
    opxy::u32 mode;       // the mode last drawn
    opxy::i32 wobble;     // the globe's nod with Drift, Q10 tenths
    opxy::i32 spin;       // the globe's turn, Q10 tenths
    opxy::i32 sway;       // Droop's slow sway (Cloud), Q15 of its 4 s
    opxy::u32 key;        // the outlines' next key
    opxy::i32 day_radius; // the radius the day side was last drawn at
    // Spring: its cycle (2^32 a turn), when its next grain is due (Q16 s) and its grains.
    opxy::u32 spring_phase;
    opxy::i32 spring_wait;
    Mark marks[48];
    opxy::u32 marks_next;
    // Cloud: when the next grain is due (Q10) and its systems.
    opxy::i32 due;
    System systems[16];
    opxy::u32 systems_next;
    opxy::u32 cloud_turn;  // which half of its work Cloud does this frame
    opxy::i32 field[21 * 21];  // the pressure over a 20 x 20 grid across the disc, Q12
    // Stretch: the recording's newest moment and the play point (Q10 s), and the
    // play point as shown: running forward to a jump over 0.45 s.
    opxy::i32 write, head, shown, leap_from;
    opxy::u32 leap_at;
    opxy::u32 leaping;
    // Slice: the slice being shown (when it began, how long it lasts, Q10 s), its piece,
    // the next slice's wait (Q10 s) and the sound's slice count.
    opxy::u32 slice_born;
    opxy::i32 slice_length, slice_wait;
    opxy::u32 piece, slice, slice_shown;
};
}
