#include "../../common/ubiqos_stdio.h"

UBIQOS_LIBC_DEFINE

// fbdemo -- takes the HDMI picture as pixels and draws a test card on it:
// colour bars, all 256 colours, ramps of red, green and blue, a fan of lines,
// a circle, and a square that bounces until the time is up.
//
//     fbdemo [SECONDS]        20 if not given; ctrl-C ends it sooner
//
// The worked example of ubiqos_video_take_framebuffer. It needs a build whose
// picture is pixels, UBIQOS_VIDEO=psram (or framebuffer); on the character
// generator there are none to take and it says so.
//
// A one-pixel white frame runs round the very edge of the picture. If any of
// it is missing on the monitor, the monitor is cropping (overscan).
//
// The picture goes back to the console when this ends, however it ends: the
// kernel takes it back from a process that is gone.

static ubiqos_framebuffer_t fb;

// RGB332: red in bits 7-5, green in 4-2, blue in 1-0.
#define RGB(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

static inline void put(int32_t x, int32_t y, uint8_t c) {
    if (x < 0 || y < 0 || x >= (int32_t)fb.width || y >= (int32_t)fb.height) return;
    ((volatile uint8_t *)fb.pixels)[(uint32_t)y * fb.stride + (uint32_t)x] = c;
}

// A filled rectangle, a word at a time in the middle: in the PSRAM build every
// store is a bus transaction of its own, and a word costs what a byte does.
static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t c) {
    if (x >= fb.width || y >= fb.height) return;
    if (x + w > fb.width) w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;
    const uint32_t word = (uint32_t)c * 0x01010101u;
    for (uint32_t row = y; row < y + h; row++) {
        volatile uint8_t *p = (volatile uint8_t *)fb.pixels + row * fb.stride + x;
        uint32_t n = w;
        while (n && ((uintptr_t)p & 3u)) { *p++ = c; n--; }
        volatile uint32_t *q = (volatile uint32_t *)p;
        while (n >= 4) { *q++ = word; n -= 4; }
        p = (volatile uint8_t *)q;
        while (n--) *p++ = c;
    }
}

// Bresenham.
static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
    const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    const int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        put(x0, y0, c);
        if (x0 == x1 && y0 == y1) return;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// The midpoint circle, eight points a step.
static void circle(int32_t cx, int32_t cy, int32_t r, uint8_t c) {
    int32_t x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put(cx + x, cy + y, c); put(cx + y, cy + x, c);
        put(cx - y, cy + x, c); put(cx - x, cy + y, c);
        put(cx - x, cy - y, c); put(cx - y, cy - x, c);
        put(cx + y, cy - x, c); put(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

// The layout is for 640x480; a larger picture gets it in its top left corner.
#define BAND_Y      428u    // where the bottom band starts
#define BOX         16u
#define BOX_X_MAX   300u    // the square bounces in the left half of the band

static void draw_card(void) {
    rect(0, 0, fb.width, fb.height, 0);

    // Bars in the classic order, 80 pixels each.
    static const uint8_t bars[8] = {
        RGB(7, 7, 3), RGB(7, 7, 0), RGB(0, 7, 3), RGB(0, 7, 0),
        RGB(7, 0, 3), RGB(7, 0, 0), RGB(0, 0, 3), RGB(0, 0, 0),
    };
    for (uint32_t i = 0; i < 8; i++) rect(i * 80, 8, 80, 72, bars[i]);

    // Every colour there is: the byte IS the colour, so cell i is colour i.
    for (uint32_t i = 0; i < 256; i++)
        rect((i % 16) * 40, 96 + (i / 16) * 16, 40, 16, (uint8_t)i);

    // Ramps: eight steps of red and of green, four of blue.
    for (uint32_t i = 0; i < 8; i++) {
        rect(i * 80, 368, 80, 16, RGB(i, 0, 0));
        rect(i * 80, 384, 80, 16, RGB(0, i, 0));
    }
    for (uint32_t i = 0; i < 4; i++) rect(i * 160, 400, 160, 16, RGB(0, 0, i));

    // A fan of lines and a circle in the right half of the bottom band.
    for (int32_t i = 0; i <= 8; i++)
        line(320, 478, 320 + i * 20, BAND_Y + 2, RGB(7, 7, 3));
    circle(560, 452, 22, RGB(0, 7, 3));
    circle(560, 452, 12, RGB(7, 7, 0));

    // The edge of the picture, one pixel wide.
    const uint8_t white = RGB(7, 7, 3);
    rect(0, 0, fb.width, 1, white);
    rect(0, fb.height - 1, fb.width, 1, white);
    rect(0, 0, 1, fb.height, white);
    rect(fb.width - 1, 0, 1, fb.height, white);
}

static uint32_t num(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return *s ? 0 : v;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: fbdemo [SECONDS]\n\n"
            "Takes the HDMI picture as pixels and draws a test card on it: colour\n"
            "bars, all 256 colours, ramps, lines, a circle and a bouncing square.\n"
            "A one-pixel white frame marks the edge of the picture. SECONDS is how\n"
            "long it stays, 20 if not given; ctrl-C ends it sooner. Needs a build\n"
            "whose picture is pixels (UBIQOS_VIDEO=psram).\n")) return;
    uint32_t secs = 20;
    if (argc == 2) secs = num(argv[1]);
    if (argc > 2 || !secs || secs > 3600) {
        printf("usage: fbdemo [SECONDS], 1..3600\n");
        return;
    }
    if (!ubiqos_video_take_framebuffer(&fb)) {
        printf("fbdemo: no picture to draw on -- this build's display is character\n"
               "cells, or another program has it\n");
        return;
    }
    // The console may be half way through a character when it is told to
    // stop; it stops within one of its own sleeps.
    ubiqos_sleep(5);

    const uint32_t t0 = ubiqos_ticks_now();
    draw_card();
    const uint32_t drew = ubiqos_ticks_now() - t0;

    // The square, erased and drawn again every frame or so.
    int32_t x = 4, y = BAND_Y + 4, dx = 3, dy = 2;
    const uint32_t end = ubiqos_ticks_now() + secs * 1000u;
    uint32_t frames = 0;
    while ((int32_t)(end - ubiqos_ticks_now()) > 0) {
        rect((uint32_t)x, (uint32_t)y, BOX, BOX, 0);
        x += dx; y += dy;
        if (x < 4 || x + (int32_t)BOX > (int32_t)BOX_X_MAX) { dx = -dx; x += 2 * dx; }
        if (y < (int32_t)BAND_Y + 4 || y + (int32_t)BOX > (int32_t)fb.height - 4) { dy = -dy; y += 2 * dy; }
        rect((uint32_t)x, (uint32_t)y, BOX, BOX, RGB(7, 3, 0));
        frames++;
        ubiqos_sleep(16);
    }

    ubiqos_video_give_framebuffer();
    printf("fbdemo: %ux%u, drew the card in %u ms, then %u frames of the square\n",
           (unsigned)fb.width, (unsigned)fb.height, (unsigned)drew, (unsigned)frames);
}
