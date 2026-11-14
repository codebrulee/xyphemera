#pragma once
#include <opxy/effect.hpp>
#include <opxy/transport.hpp>

// Fragmented Earth: the granular looper.
namespace fragmentedearth {
// The whole send buffer (512 KB): the grain pool, then scratch, then the recording.
const opxy::u32 kBufferWords = opxy::effect_buffer_bytes / 4;

// At most effect_buffered_state_bytes less the shift row's 4 bytes.
struct State {
    opxy::u32 ready;      // the grain pool has been set up in the buffer
    opxy::u32 random;     // xorshift
    opxy::u32 clock;      // samples since initialize
    opxy::u32 write;      // the recording's write position
    opxy::u32 filled;     // samples recorded since initialize, up to the recording's length
    opxy::u32 recorded;   // samples written since the current phrase began
    opxy::u32 last_loud;  // clock at the input's last loud block
    opxy::u32 frozen;     // holding a phrase: the recording stops, the grains read the region
    opxy::u32 draining;   // just held: still recording until the grains that were playing finish
    opxy::u32 region_start, region_length, rewrite;
    opxy::u32 spawn;      // grains due, Q16
    opxy::u32 head;       // Stretch: where it plays, in the recording, Q8 samples
    opxy::i32 spring_wait;     // Spring: samples to the next grain
    opxy::u32 spring_phase;    // where it is in its expanding and contracting, a whole cycle 2^32
    opxy::u32 spring_anchor;   // where it plays, in the recording
    opxy::u32 spring_elapsed;  // samples since the cycle began
    opxy::u32 spring_tick;     // the song position it last followed
    opxy::i32 level;      // the input's level, Q15
    opxy::i32 tone;       // one-pole on what Repeats feeds back, Q23
    opxy::i32 dc;         // the DC in what Repeats feeds back, Q23 (taken out)
    opxy::i32 loudest;    // the phrase's loudest level as it was played, Q15 (as level)
    opxy::i32 tone_level; // the level of what Repeats feeds back, Q15 (as level)
    opxy::i32 held_level; // the held phrase's level where Repeats rewrites it, Q15 (as level)
    opxy::i32 lift;       // what Repeats' rewrite is brought up by, Q12, moving smoothly
    opxy::i32 trim;       // and the phrase turned down by while too loud, Q15
    opxy::i32 fade;       // Hold: the held drone's level, Q23, falling over Hold's time
    opxy::u32 sway;       // Cloud and Stretch: the slow cycle Droop follows, a whole cycle 2^32
    opxy::i32 grid_wait;  // on tempo, stopped: samples to the next grain on the note grid
    opxy::u32 grid_note;  // on tempo, playing: the note of the grid it last played on
    // Slice: the onsets found while recording (their write counts are in the buffer), the
    // recording's write count, the level the onsets are found against, and the slice playing.
    opxy::u32 written;    // samples written since initialize
    opxy::u32 onsets;     // onsets found since initialize
    opxy::u32 last_onset; // `written` at the last onset
    opxy::u32 stretch_onset; // the onsets Stretch has followed
    opxy::i32 onset_level;  // Q15: rising over 20 ms, falling over 50 ms
    opxy::i32 slice_wait; // free: samples to the next slice
    opxy::u32 slice;      // the last slice played
    // Space: where its lines are (all eight turn together through its part of the buffer),
    // the loop's damping, and how much of the buffer it has cleared since initialize.
    opxy::u32 space_at;
    opxy::i32 space_damp[4];
    opxy::i32 space_last[2];  // its last output each side, to interpolate from
    opxy::u32 space_cleared;
};

void initialize(State& state);
void process(State& state, opxy::Buffer buffer, const opxy::Transport& transport, opxy::Channel left,
             opxy::Channel right, opxy::Parameters parameters);
}
