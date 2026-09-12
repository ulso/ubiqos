// No console, because there is no video to put one on.
//
// A board whose panel has no driver yet -- MYRTOS_VIDEO=none, see the CMake
// block that chooses it -- builds without video.c, chargen.c and console.c.
// But the console's API is called from four other places: myrtos_print and
// myrtos_putc in main.c, the trap handler's trace in syscalls.c, and the `con`
// device in io.c. Guarding each of those would put the absence of a display
// into files that have nothing to do with displays.
//
// So the API stays and answers honestly instead. Every call here says "nothing
// went anywhere", which is true, and the callers already handle it: myrtos_print
// still fills the dmesg ring, so a board with no screen and no serial line is
// still readable over the debug probe -- which is how the first boot on the
// Waveshare board was watched.
//
// This file is a promise that the real console will be dropped in beside it
// rather than worked around. When there is an ST7262 driver, MYRTOS_VIDEO stops
// being `none` and this file stops being compiled.

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"

void myrtos_console_init(void) { }
void myrtos_console_start_server(void) { }

void myrtos_console_putc(char c) { (void)c; }
void myrtos_console_write(const char *p, uint32_t n) { (void)p; (void)n; }

// Nothing is written, so nothing is ever accepted: a writer that checks first
// is told there is no room, and one that does not is told it wrote nothing.
uint32_t myrtos_console_room(void) { return 0; }
uint32_t myrtos_console_put(const uint8_t *buf, uint32_t len) { (void)buf; (void)len; return 0; }

// The scrollback the trap handler reads for its trace. Empty, and -1 is what
// "past the end" already means to every caller of it.
uint32_t myrtos_console_trace_size(void) { return 0; }
int32_t  myrtos_console_trace_at(uint32_t offset) { (void)offset; return -1; }

// No fonts either. -1 rather than 0, because 0 would claim a font was selected.
int32_t myrtos_console_select_font(int32_t index, myrtos_confont_t *out, bool look_only)
{
    (void)index; (void)out; (void)look_only;
    return -1;
}
