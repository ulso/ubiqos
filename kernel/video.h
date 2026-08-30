// The display, and the text console drawn on it.
#ifndef MYRTOS_VIDEO_H
#define MYRTOS_VIDEO_H

#include <stdint.h>

#define MYRTOS_H_ACTIVE 640
#define MYRTOS_V_ACTIVE 480

void myrtos_video_init(void);
void myrtos_video_testcard(void);

extern uint8_t *myrtos_framebuf;

// The framebuffer is a ring of scanlines and this is the one shown at the top.
// Scrolling moves it rather than moving 300 kB of pixels: the display is played
// from a table of line addresses, so rotating the table is the whole job. It is
// 480 stores in SRAM against 300 kB of read-modify-write in PSRAM, which is the
// difference between a scroll nobody notices and one that stutters.
extern uint32_t myrtos_video_origin;
void myrtos_video_set_origin(uint32_t line);

void myrtos_console_init(void);
void myrtos_console_putc(char c);

#endif
