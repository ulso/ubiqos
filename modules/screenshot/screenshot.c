#include "../../common/myrtos_stdio.h"

// screenshot -- what the panel shows, as a BMP file on the card.
//
//     screenshot                  the next free /sd/shotNNN.bmp
//     screenshot /sd/list.bmp     a name of your own, which must not exist yet
//
// For showing a layout to somebody who is not standing in front of the board,
// which a phone photograph of a dark screen does badly.
//
// The whole picture is taken first and written afterwards. Each line comes from
// the kernel rendered exactly as the panel shows it -- the same rasteriser the
// video interrupt runs, or the character generator when no scene is set -- and
// 480 of them take a fraction of a second, so an application redrawing its
// screen every second and a half is caught between redraws rather than half in
// each. The card is slower, and nothing on it can change the picture.
//
// Eight bits a pixel with a palette, when the screen has 255 colours or fewer:
// a third of the size of true colour, and every screen airview draws has a
// dozen. Anything with more falls back to 24 bits, taken line by line while it
// is written. Both are plain uncompressed BMP, which opens anywhere without a
// converter.
//
// A file that exists is never written over. The filesystem does not truncate,
// so a shorter picture written over a longer file would leave the old tail
// behind it -- an image that opens and is wrong.

MYRTOS_LIBC_DEFINE
MYRTOS_MEM_SIZE(16384);

#define W 800u
#define H 480u
#define ROWS_A_WRITE 12u

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffffu); put16(p + 2, v >> 16); }

// RGB565 to the three bytes BMP wants, blue first. The low bits are filled from
// the high ones, so white comes out 255 and not 248.
static void to_bgr(uint16_t c, uint8_t *bgr)
{
    const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    bgr[0] = (uint8_t)((b << 3) | (b >> 2));
    bgr[1] = (uint8_t)((g << 2) | (g >> 4));
    bgr[2] = (uint8_t)((r << 3) | (r >> 2));
}

static bool write_all(int fd, const uint8_t *p, uint32_t n)
{
    while (n) {
        const int32_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (uint32_t)w;
    }
    return true;
}

// The two headers. Rows run bottom to top, as a positive height says, and both
// row widths -- 800 and 2400 bytes -- are multiples of four, so there is no
// padding to add.
static uint32_t make_header(uint8_t *h, uint32_t bpp, uint32_t colours)
{
    const uint32_t palette = bpp == 8u ? 4u * 256u : 0u;
    const uint32_t offset = 14u + 40u + palette;
    const uint32_t image = W * (bpp / 8u) * H;
    for (uint32_t i = 0; i < 54u; i++) h[i] = 0;
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, offset + image);
    put32(h + 10, offset);
    put32(h + 14, 40u);
    put32(h + 18, W);
    put32(h + 22, H);
    put16(h + 26, 1u);
    put16(h + 28, bpp);
    put32(h + 34, image);
    put32(h + 38, 2835u);                  // 72 dpi, for whatever asks
    put32(h + 42, 2835u);
    put32(h + 46, bpp == 8u ? colours : 0u);
    return offset + image;
}

void module_main(int argc, char **argv)
{
    if (myrtos_help(argc, argv,
            "usage: screenshot [FILE]\n\n"
            "Saves what the panel shows as a BMP: FILE, which must not exist yet,\n"
            "or the next free /sd/shotNNN.bmp. 800 x 480, eight bits a pixel with\n"
            "a palette when the screen has 255 colours or fewer, 24 otherwise.\n"))
        return;

    char path[64];
    struct stat st;
    if (argc > 1) {
        uint32_t n = 0;
        while (argv[1][n] && n < sizeof path - 1) { path[n] = argv[1][n]; n++; }
        path[n] = 0;
        if (stat(path, &st) == 0) {
            printf("screenshot: %s exists, and is not written over\n", path);
            return;
        }
    } else {
        uint32_t k = 1;
        for (; k < 1000u; k++) {
            snprintf(path, sizeof path, "/sd/shot%03lu.bmp", (unsigned long)k);
            if (stat(path, &st) != 0) break;
        }
        if (k == 1000u) { printf("screenshot: shot001 to shot999 are all taken\n"); return; }
    }

    uint16_t line[W];
    if (myrtos_video_capture(0, line) < 0) {
        printf("screenshot: this display cannot be captured\n");
        return;
    }

    uint8_t *pixels = myrtos_alloc_bulk(W * H);
    uint8_t *index  = myrtos_alloc_bulk(65536u);    // 0 unseen, else palette slot + 1
    uint8_t *chunk  = myrtos_alloc_bulk(W * 3u * ROWS_A_WRITE);
    if (!pixels || !index || !chunk) {
        printf("screenshot: no memory for the picture\n");
        if (pixels) myrtos_free(pixels);
        if (index)  myrtos_free(index);
        if (chunk)  myrtos_free(chunk);
        return;
    }
    for (uint32_t i = 0; i < 65536u; i++) index[i] = 0;

    uint16_t palette[255];
    uint32_t colours = 0;
    bool fits = true;

    const uint32_t t0 = myrtos_ticks_now();
    for (uint32_t y = 0; y < H && fits; y++) {
        myrtos_video_capture(y, line);
        for (uint32_t x = 0; x < W; x++) {
            const uint16_t c = line[x];
            uint32_t slot = index[c];
            if (!slot) {
                if (colours == 255u) { fits = false; break; }
                palette[colours++] = c;
                index[c] = (uint8_t)colours;
                slot = colours;
            }
            pixels[y * W + x] = (uint8_t)(slot - 1u);
        }
    }
    const uint32_t t1 = myrtos_ticks_now();

    const int fd = open(path, O_WRONLY | O_CREAT);
    if (fd < 0) {
        printf("screenshot: cannot write %s -- is there a card?\n", path);
        myrtos_free(pixels); myrtos_free(index); myrtos_free(chunk);
        return;
    }

    uint8_t head[54];
    uint32_t size;
    bool ok;
    if (fits) {
        size = make_header(head, 8u, colours);
        ok = write_all(fd, head, sizeof head);
        uint8_t pal[4u * 256u];
        for (uint32_t i = 0; i < sizeof pal; i++) pal[i] = 0;
        for (uint32_t i = 0; i < colours; i++) to_bgr(palette[i], pal + 4u * i);
        ok = ok && write_all(fd, pal, sizeof pal);

        for (uint32_t r = 0; ok && r < H; r += ROWS_A_WRITE) {
            const uint32_t n = H - r < ROWS_A_WRITE ? H - r : ROWS_A_WRITE;
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t *src = pixels + (H - 1u - r - k) * W;
                for (uint32_t x = 0; x < W; x++) chunk[k * W + x] = src[x];
            }
            ok = write_all(fd, chunk, n * W);
        }
    } else {
        // More colours than a palette holds: true colour, each line taken
        // again as it is written, bottom first.
        colours = 0;
        size = make_header(head, 24u, 0u);
        ok = write_all(fd, head, sizeof head);
        for (uint32_t r = 0; ok && r < H; r += ROWS_A_WRITE) {
            const uint32_t n = H - r < ROWS_A_WRITE ? H - r : ROWS_A_WRITE;
            for (uint32_t k = 0; k < n; k++) {
                myrtos_video_capture(H - 1u - r - k, line);
                for (uint32_t x = 0; x < W; x++) to_bgr(line[x], chunk + (k * W + x) * 3u);
            }
            ok = write_all(fd, chunk, n * W * 3u);
        }
    }
    close(fd);
    const uint32_t t2 = myrtos_ticks_now();

    myrtos_free(pixels);
    myrtos_free(index);
    myrtos_free(chunk);

    if (!ok) {
        printf("screenshot: writing %s failed part way; the file is incomplete\n", path);
        return;
    }
    if (fits)
        printf("screenshot: %s, %lu colours, %lu kB; taken in %lu ms, written in %lu ms\n",
               path, (unsigned long)colours, (unsigned long)(size / 1024u),
               (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
    else
        printf("screenshot: %s, true colour, %lu kB; written in %lu ms\n",
               path, (unsigned long)(size / 1024u), (unsigned long)(t2 - t1));
}
