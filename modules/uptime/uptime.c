#include "../../common/ubiqos_stdio.h"

UBIQOS_LIBC_DEFINE

// uptime -- how long since the machine started, and when that was.
//
//     up 18 h 23 min, since 2026-09-26 15:24
//
// Both halves come from /var, as `date` does: /var/uptime is seconds since
// reset from the hardware timer, and /var/time is the wall clock, once the
// network has said what it is. The start is the one minus the other. Before the
// clock is set only the first half is known, and only the first half is said.
//
// It is also the key to /var/dmesg, whose stamps count from the same moment:
// the start plus a stamp is when a line was written.

static uint32_t read_line(const char *path, char *out, uint32_t cap) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const int n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return 0;
    out[n] = 0;
    return (uint32_t)n;
}

// Days since 1970-01-01 from a civil date, and back -- Howard Hinnant's
// algorithms, the same the kernel's clock uses.
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m > 2 ? m - 3 : m + 9) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int32_t)yoe + era * 400 + (*m <= 2);
}

static uint32_t num(const char *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v * 10u + (uint32_t)(p[i] - '0');
    return v;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: uptime\n\n"
            "How long since the machine started, and -- once the network has\n"
            "said what time it is -- when that was. The stamps in /var/dmesg\n"
            "count from the same moment.\n"))
        return;

    char up[24];
    if (!read_line("/var/uptime", up, sizeof up)) {
        ubiqos_write_str(UBIQOS_STDERR, "uptime: this kernel has no /var/uptime\n");
        return;
    }
    uint32_t secs = 0;
    for (const char *p = up; *p >= '0' && *p <= '9'; p++) secs = secs * 10u + (uint32_t)(*p - '0');

    const uint32_t days = secs / 86400u, h = secs / 3600u % 24u, m = secs / 60u % 60u, s = secs % 60u;
    // The two largest units that are not zero, which is all anyone reads.
    if (days)   printf("up %lu day%s %lu h", (unsigned long)days, days == 1 ? "" : "s", (unsigned long)h);
    else if (h) printf("up %lu h %lu min", (unsigned long)h, (unsigned long)m);
    else if (m) printf("up %lu min %lu s", (unsigned long)m, (unsigned long)s);
    else        printf("up %lu s", (unsigned long)s);

    // "2026-09-27 09:43:09" when the clock is set, dashes when it is not.
    char now[32];
    if (read_line("/var/time", now, sizeof now) >= 19 && now[4] == '-' && now[0] >= '0' && now[0] <= '9') {
        const int32_t day = days_from_civil((int32_t)num(now, 4), num(now + 5, 2), num(now + 8, 2));
        // Seconds since 1970 fit an unsigned 32 bits until 2106, which keeps
        // this clear of 64-bit division -- a module has no library for it.
        const uint32_t local = (uint32_t)day * 86400u + num(now + 11, 2) * 3600u
                             + num(now + 14, 2) * 60u + num(now + 17, 2) - secs;
        int32_t y; uint32_t mo, d;
        civil_from_days((int32_t)(local / 86400u), &y, &mo, &d);
        const uint32_t sod = local % 86400u;
        printf(", since %04ld-%02lu-%02lu %02lu:%02lu", (long)y, (unsigned long)mo, (unsigned long)d,
               (unsigned long)(sod / 3600u), (unsigned long)(sod / 60u % 60u));
    }
    printf("\n");
}
