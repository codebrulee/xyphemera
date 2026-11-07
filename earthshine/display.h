#pragma once
#include <opxy/page_header.hpp>

namespace earthshine {
// Matches resources.display in definition.json: paths, points, nodes, texts,
// palette. tools/make_panel.py writes both and names every node (panel.h).
using Frame = opxy::draw::Frame<4, 64, 15, 18, 5>;
// The two-page header; Light, Shimmer, Dry and Mod as the screen shows them,
// easing toward their values (Q15); the Light and shift choices last seen; the
// Earth's turn and where it is heading (Q20 turns); the axis wobble's phase,
// 2^32 a cycle; whether the first frame has been drawn.
struct DisplayState {
    opxy::draw::PageHeader header;
    opxy::i32 shown[4];
    opxy::i32 last_light;
    opxy::u32 last_choices;
    opxy::i32 spin, target;
    opxy::u32 wobble;
    opxy::u32 ready;
};
}
