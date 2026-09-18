// A very small printf, for third-party code that expects one.
//
// The SDIO driver is vendored unchanged and prints on its error paths -- "bad
// response from card" and the like -- which are exactly the lines worth having
// during bring-up. Rather than edit somebody else's source, the build points its
// printf at this one, and it goes wherever every other kernel line goes.
//
// It understands what that driver actually uses: %d, %u, %s, %c, %x and %08x.
// Anything else is copied through, which is honest -- a format this does not
// know shows up as itself rather than as silence or a crash.
//
// Collected and handed over through ubiqos_print, not a character at a time
// through ubiqos_putc. putc reaches the screen and the UART but not the kernel
// log, and the Waveshare board has no UART and, with an application on its
// panel, no visible console either -- so all that reached /var/dmesg of
// "sd: gave up waiting for a DMA channel to finish (sm 1 at 2)" was the one
// part that came through %s, and the state the message exists to report was
// lost.

#include <stdint.h>
#include <stdarg.h>

void ubiqos_print(const char *s);

static char out_buf[96];
static uint32_t out_len;

static void out_flush(void) {
    if (!out_len) return;
    out_buf[out_len] = 0;
    ubiqos_print(out_buf);
    out_len = 0;
}

static void out(char c) {
    if (out_len == sizeof(out_buf) - 1) out_flush();
    out_buf[out_len++] = c;
    if (c == '\n') out_flush();
}

static void put_u32(uint32_t v, uint32_t base, uint32_t width, char pad) {
    char buf[12];
    uint32_t n = 0;
    do {
        uint32_t d = v % base;
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v && n < sizeof(buf));
    while (n < width && n < sizeof(buf)) buf[n++] = pad;
    while (n) out(buf[--n]);
}

void ubiqos_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out(*p); continue; }
        p++;

        char pad = ' ';
        uint32_t width = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (uint32_t)(*p - '0'); p++; }
        while (*p == 'l') p++;                   // long is the same width here

        switch (*p) {
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) { out('-'); put_u32((uint32_t)-v, 10, width, pad); }
            else put_u32((uint32_t)v, 10, width, pad);
            break;
        }
        case 'u': put_u32(va_arg(ap, uint32_t), 10, width, pad); break;
        case 'x': put_u32(va_arg(ap, uint32_t), 16, width, pad); break;
        case 'c': out((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char*);
            for (s = s ? s : "(null)"; *s; s++) out(*s);
            break;
        }
        case '%': out('%'); break;
        case 0:   p--; break;                    // a trailing percent
        default:  out('%'); out(*p); break;
        }
    }
    va_end(ap);
    out_flush();                                 // a line may be built in several calls; each goes as it is
}
