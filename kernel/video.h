// The display, and the text console drawn on it.
#ifndef UBIQOS_VIDEO_H
#define UBIQOS_VIDEO_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"   // ubiqos_confont_t, through the shared ABI

// Set by CMake's UBIQOS_VIDEO. 0 means a bitmap in SRAM; 1 means character
// cells and a scanline built as the display asks for it.
#ifndef UBIQOS_VIDEO_CHARGEN
#define UBIQOS_VIDEO_CHARGEN 0
#endif

#define UBIQOS_H_ACTIVE 640
#define UBIQOS_V_ACTIVE 480

void ubiqos_video_init(void);

#if UBIQOS_VIDEO_CHARGEN

// How far ahead of the beam the generator has to stay, and how it is doing.
// underruns is the number that decides whether this design holds: it counts
// the times a scanline was still unwritten when the display reached it.
extern uint32_t ubiqos_video_underruns, ubiqos_video_pumps, ubiqos_video_lines;
uint32_t ubiqos_video_buffers(void);
void ubiqos_video_stats_fill(uint32_t *sixteen);
void ubiqos_video_peek_line(uint32_t line, uint8_t *out, uint32_t n);

#else

void ubiqos_video_testcard(void);

extern uint8_t *ubiqos_framebuf;

// The framebuffer is a ring of scanlines and this is the one shown at the top.
// Scrolling moves it rather than moving 300 kB of pixels: the display is played
// from a table of line addresses, so rotating the table is the whole job. It is
// 480 stores in SRAM against 300 kB of read-modify-write in PSRAM, which is the
// difference between a scroll nobody notices and one that stutters.
extern uint32_t ubiqos_video_origin;
void ubiqos_video_set_origin(uint32_t line);

#endif

void ubiqos_console_init(void);
void ubiqos_console_putc(char c);
void ubiqos_console_start_server(void);
uint32_t ubiqos_console_put(const uint8_t *buf, uint32_t len);
uint32_t ubiqos_console_room(void);
void     ubiqos_console_write(const char *p, uint32_t n);

// Which font the console draws in. The switch is applied by the console's own
// server thread once it has drawn everything already queued, so this only
// records the wish -- see console.c.
int32_t ubiqos_console_select_font(int32_t index, ubiqos_confont_t *out,
                                   bool look_only);

// Below the USB task: the keyboard must never wait behind pixels. Above a shell,
// so what has been printed reaches the screen rather than queuing behind the
// process that printed it.
#define UBIQOS_PRIO_CONSOLE 24

#endif
