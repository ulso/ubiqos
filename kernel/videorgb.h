// The geometry of a parallel RGB panel, in the shape the character generator
// expects. kernel/video.h says the same things about DVI out of HSTX; chargen.h
// includes whichever applies.
//
// Two differences from that one, and they are the whole of what made the
// character generator board-specific: this panel is wider, and its pixels are
// two bytes rather than one.
#ifndef MYRTOS_VIDEORGB_H
#define MYRTOS_VIDEORGB_H

#define MYRTOS_H_ACTIVE 800
#define MYRTOS_V_ACTIVE 480

// 800 / 8 = 100 columns, 480 / 16 = 30 rows. Both exact, which is luck worth
// noticing: the cell size did not have to be reconsidered.

#endif
