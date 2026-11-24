// Blixt in a Mac or Linux terminal: everything but running the program,
// which is Thumb-2 for UbiqOS. F9 writes the module next to the source, so it
// can be carried to a board.
//
//   cc -O2 -o blixt modules/blixt/*.c     (plat_ubiqos.c excluded)
#include "plat.h"
#include "modwrite.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios saved;

void plat_init(void)
{
    tcgetattr(0, &saved);
    struct termios t = saved;
    cfmakeraw(&t);
    tcsetattr(0, TCSAFLUSH, &t);
}

void plat_done(void)
{
    tcsetattr(0, TCSAFLUSH, &saved);
}

void plat_write(const char *s, int n)
{
    while (n > 0) {
        ssize_t w = write(1, s, (size_t)n);
        if (w <= 0) return;
        s += w;
        n -= (int)w;
    }
}

int plat_read(uint8_t *buf, int max, int timeout_ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    ssize_t n = read(0, buf, (size_t)max);
    return n > 0 ? (int)n : 0;
}

uint32_t plat_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void plat_default_size(int *rows, int *cols)
{
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row && w.ws_col) {
        *rows = w.ws_row;
        *cols = w.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

char *plat_load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    b[n] = 0;
    fclose(f);
    *size = n;
    return b;
}

bool plat_save(const char *path, const char *data, long size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)size, f) == (size_t)size;
    return fclose(f) == 0 && ok;
}

bool plat_run(const char *name, const char *args, const asm_result_t *r, char *msg, int msglen)
{
    (void)args;
    uint32_t size;
    uint8_t *m = mod_build(name, r, &size);
    char path[64];
    snprintf(path, sizeof path, "%s.mod", name);
    bool ok = m && plat_save(path, (const char *)m, (long)size);
    free(m);
    if (ok) snprintf(msg, (size_t)msglen, "Wrote %s, %u bytes. Running it needs UbiqOS.", path, size);
    else snprintf(msg, (size_t)msglen, "Could not write %s.", path);
    return ok;
}

const char *plat_name(void) { return "a POSIX terminal"; }

int blixt_main(int argc, char **argv);
int main(int argc, char **argv) { return blixt_main(argc, argv); }
