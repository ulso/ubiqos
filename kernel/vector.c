#include "vector.h"
#include "video.h"

// A segment as it is stored: always top to bottom, so the active list only ever
// has to ask whether y has passed the bottom.
typedef struct {
    int16_t x0, y0, x1, y1;
    uint8_t colour;
} seg_t;

static seg_t   segs[MYRTOS_VEC_MAX];
static uint16_t order[MYRTOS_VEC_MAX];      // indices, sorted by y0
static uint16_t nsegs;

// The active list, and the running x of each entry in 16.16 fixed point. prev
// is the x it had on the line above: a segment that moves more than a pixel
// sideways per scanline would otherwise be drawn as a dotted line, which is the
// one real weakness of doing it this way and costs nothing to fix.
// One record per active segment rather than four parallel arrays. It is read
// once per scanline for every segment on the screen, so the four loads were
// four chances to miss; and the colour and bottom edge are copied in so that
// drawing a line never has to reach back into segs[] at all.
typedef struct {
    int32_t  x;          // 16.16, this scanline
    int32_t  step;       // 16.16 per scanline
    int32_t  prev;       // where it was on the line above
    int16_t  y1;
    uint8_t  colour;
} active_t;

static active_t act[MYRTOS_VEC_ACTIVE_MAX];
static uint16_t nact;
static uint16_t next_add;                   // position in order[] for this y
static uint32_t expect_y = 0xffffffffu;     // the y this state is valid for

void myrtos_vector_clear(void)
{
    nsegs = 0;
    nact = 0;
    next_add = 0;
    expect_y = 0xffffffffu;
}

uint32_t myrtos_vector_count(void) { return nsegs; }

static int16_t clamp16(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int16_t)v;
}

int32_t myrtos_vector_add(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t colour)
{
    if (nsegs >= MYRTOS_VEC_MAX)
        return -1;

    // Off the sides is clamped rather than refused, which keeps a plot that
    // runs past the edge useful instead of empty. Off the top or bottom is
    // refused, because there is no scanline to draw it on.
    if (y0 > y1) {
        int32_t t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    if (y1 < 0 || y0 >= (int32_t)MYRTOS_V_ACTIVE)
        return -1;

    seg_t *s = &segs[nsegs];
    s->x0 = clamp16(x0, -4096, 4096);
    s->x1 = clamp16(x1, -4096, 4096);
    s->y0 = clamp16(y0, 0, MYRTOS_V_ACTIVE - 1);
    s->y1 = clamp16(y1, 0, MYRTOS_V_ACTIVE - 1);
    s->colour = colour;

    // Insertion sort into order[], which is what lets the display add segments
    // by walking forward rather than searching. Adding is rare and drawing is
    // not, so the cost belongs here.
    uint16_t i = nsegs;
    while (i && segs[order[i - 1]].y0 > s->y0) {
        order[i] = order[i - 1];
        i--;
    }
    order[i] = nsegs;
    nsegs++;

    expect_y = 0xffffffffu;      // the active list no longer describes anything
    return (int32_t)(nsegs - 1);
}

// Rebuild the active list from nothing, for a y that did not follow the last
// one. The display normally walks straight down, but a missed refill makes it
// jump, and state that assumed otherwise would draw the rest of the frame wrong.
static void rebuild(uint32_t y)
{
    nact = 0;
    next_add = 0;

    for (uint16_t k = 0; k < nsegs; k++) {
        const seg_t *s = &segs[order[k]];
        if ((uint32_t)s->y0 > y) break;
        next_add = (uint16_t)(k + 1);
        if ((uint32_t)s->y1 < y) continue;
        if (nact >= MYRTOS_VEC_ACTIVE_MAX) continue;

        int32_t dy = s->y1 - s->y0;
        int32_t step = dy ? (((int32_t)s->x1 - s->x0) << 16) / dy : 0;
        int32_t x = ((int32_t)s->x0 << 16) + step * (int32_t)(y - (uint32_t)s->y0);

        act[nact].x = x;
        act[nact].step = step;
        act[nact].prev = x - step;   // as if it had been drawn on the line above
        act[nact].y1 = s->y1;
        act[nact].colour = s->colour;
        nact++;
    }
    expect_y = y;
}

static inline void span(uint8_t *dst, int32_t a, int32_t b, uint8_t c)
{
    // One pixel is the common case and it used to cost five comparisons, a
    // loop setup and a loop test to store one byte. Anything steeper than 45
    // degrees moves less than a pixel sideways per scanline, and a vertical
    // segment moves none. The cast makes a negative a large unsigned, so the
    // one test covers both ends.
    if (a == b) {
        if ((uint32_t)a < (uint32_t)MYRTOS_H_ACTIVE) dst[a] = c;
        return;
    }

    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b < 0 || a >= (int32_t)MYRTOS_H_ACTIVE) return;
    if (a < 0) a = 0;
    if (b >= (int32_t)MYRTOS_H_ACTIVE) b = MYRTOS_H_ACTIVE - 1;
    for (int32_t x = a; x <= b; x++)
        dst[x] = c;
}

void myrtos_vector_line(uint32_t y, uint8_t *dst)
{
    if (!nsegs)
        return;                      // one compare when there is nothing to draw

    if (y != expect_y) {
        rebuild(y);
    } else {
        // Take up what starts here. order[] is sorted, so this walks forward.
        while (next_add < nsegs && (uint32_t)segs[order[next_add]].y0 <= y) {
            const seg_t *s = &segs[order[next_add]];
            if ((uint32_t)s->y1 >= y && nact < MYRTOS_VEC_ACTIVE_MAX) {
                int32_t dy = s->y1 - s->y0;
                int32_t step = dy ? (((int32_t)s->x1 - s->x0) << 16) / dy : 0;
                act[nact].x = (int32_t)s->x0 << 16;
                act[nact].step = step;
                act[nact].prev = act[nact].x;
                act[nact].y1 = s->y1;
                act[nact].colour = s->colour;
                nact++;
            }
            next_add++;
        }
    }

    // Draw, advance and drop in one pass. The compaction used to be a second
    // pass that copied every entry every scanline whether or not anything had
    // expired, which is most of what a segment cost.
    uint16_t w = 0;
    for (uint16_t i = 0; i < nact; i++) {
        if ((uint32_t)act[i].y1 < y)
            continue;
        if (w != i)
            act[w] = act[i];

        span(dst, act[w].prev >> 16, act[w].x >> 16, act[w].colour);
        act[w].prev = act[w].x;
        act[w].x += act[w].step;
        w++;
    }
    nact = w;

    expect_y = y + 1;
}
