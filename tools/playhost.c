// Runs modules/play/play.c on the host, unmodified, with the system calls
// stubbed onto real files. The point is to test the code that ships rather
// than a copy of its arithmetic.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MYRTOS_ABI_H            // keep the real header out
typedef struct { char *p; } myrtos_line_t;
#define MYRTOS_STDOUT 1
#define MYRTOS_STDERR 2
#define MYRTOS_O_RDONLY 0u
#define MYRTOS_SEEK_CUR 1u
#define MYRTOS_SS_RATE 0x0001u
#define MYRTOS_SS_VOLUME 0x0100u
#define MYRTOS_MEM_SIZE(n)

static FILE *in_f;
static FILE *out_f;
static uint32_t g_dst_rate;

static int32_t myrtos_open_flags(const char *n, uint32_t f) { (void)f; return in_f ? 3 : -1; }
static int32_t myrtos_open(const char *n) { return strcmp(n, "/dev/audio") ? -1 : 4; }
static int32_t myrtos_read(int32_t fd, void *b, uint32_t n) { return (int32_t)fread(b, 1, n, in_f); }
static int32_t myrtos_write(int32_t fd, const void *b, uint32_t n) {
    // Short writes on purpose, and an awkward number: this is exactly what
    // /dev/audio does, and a resampler that only works when every write is
    // taken whole would pass a test that took them whole.
    uint32_t take = n > 700u ? 700u : n;
    fwrite(b, 1, take, out_f);
    return (int32_t)take;
}
static int32_t myrtos_seek(int32_t fd, int32_t off, uint32_t wh) { return fseek(in_f, off, SEEK_CUR); }
static int32_t myrtos_close(int32_t fd) { return 0; }
static void    myrtos_sleep(uint32_t ms) { (void)ms; }
static int32_t myrtos_getstat(int32_t fd, uint32_t c, void *d, uint32_t l) {
    if (c != MYRTOS_SS_RATE) return -1;
    *(uint32_t*)d = g_dst_rate; return 0;
}
static bool myrtos_help(int argc, char **argv, const char *t) { (void)argc;(void)argv;(void)t; return false; }
static char linebuf[512]; static int linelen;
static void myrtos_line_reset(myrtos_line_t *l) { (void)l; linelen = 0; linebuf[0] = 0; }
static void myrtos_line_str(myrtos_line_t *l, const char *s) { (void)l; linelen += snprintf(linebuf+linelen, sizeof linebuf-linelen, "%s", s); }
static void myrtos_line_u32(myrtos_line_t *l, uint32_t v) { (void)l; linelen += snprintf(linebuf+linelen, sizeof linebuf-linelen, "%u", v); }
static void myrtos_line_flush(int32_t fd, myrtos_line_t *l) { (void)l; fputs(linebuf, stderr); linelen = 0; }

// The module itself, with its one include stripped -- the stubs above stand in
// for it. Built by:
//
//   sed 1d ../modules/play/play.c > play_body.h
//   cc -O2 -o playhost playhost.c
//   ./playhost 48000 in.wav out.pcm
//
// out.pcm is raw 16-bit stereo at the rate given, which is what /dev/audio
// takes, so anything that can look at a spectrum can say whether the resampler
// is right. Listening cannot: a resampler with the wrong step sounds like
// music at the wrong pitch, and one that mishandles a buffer boundary sounds
// fine until you look.
#include "play_body.h"

int main(int argc, char **argv) {
    g_dst_rate = (uint32_t)atoi(argv[1]);
    in_f = fopen(argv[2], "rb");
    out_f = fopen(argv[3], "wb");
    char *av[] = { "play", argv[2], 0 };
    module_main(2, av);
    fclose(out_f);
    return 0;
}
