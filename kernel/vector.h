// Vector graphics with no framebuffer to draw them into.
//
// The list is kept sorted by the topmost end of each segment, and the display
// walks it once per frame: at each scanline the segments that start there join
// an active list, those that have ended leave it, and every one still in it has
// its x advanced by dx/dy. That is the scanline algorithm from Newman and
// Sproull, and the reason to use it here is the same reason it was used then --
// there is nowhere to keep a picture, only time to build one line at a time.
//
// It composes over the character generator rather than replacing it: the line
// is built from cells first and the segments are drawn on top, so a program can
// put a label beside a plot without either knowing about the other.
#ifndef MYRTOS_VECTOR_H
#define MYRTOS_VECTOR_H

#include <stdint.h>
#include <stdbool.h>

#define MYRTOS_VEC_MAX 256

// How many segments may cross one scanline. This is the real limit, not the
// list size: the display builds a line every 32 microseconds and a segment
// costs about 1.5 per cent of the processor for every scanline it crosses, so
// an unbounded active list lets a picture starve the machine that draws it.
// Over the cap, segments are simply not taken up -- the picture loses lines and
// the machine keeps going, which is the right way round.
//
// The number belongs to whoever is building the machine, not to this file.
// MYRTOS_VEC_ACTIVE in CMake sets it; 64 is what fits beside a text console at
// 49 per cent, and roughly twice that fits with the text turned off. There is
// no safe value that is also universal, which is exactly why it is a knob.
#ifndef MYRTOS_VEC_ACTIVE_MAX
#define MYRTOS_VEC_ACTIVE_MAX 64
#endif

int32_t  myrtos_vector_add(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t colour);
void     myrtos_vector_clear(void);
uint32_t myrtos_vector_count(void);

// One scanline, drawn over whatever is already in dst. Called from the display
// interrupt, so it touches nothing but its own state.
void myrtos_vector_line(uint32_t y, uint8_t *dst);

#endif
