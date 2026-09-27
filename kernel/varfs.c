#include "vfs.h"
#include "../common/ubiqos_abi.h"

// /var -- what the kernel has said, as a file.
//
// One file and read only. It exists because the boot messages go to the screen
// and the UART, and a session on the USB console never sees them: by the time
// that console exists the kernel has already said everything interesting. This
// is where to read it afterwards.
//
// The ring lives in main.c beside ubiqos_print, because the first line is
// written before any of this is running.
//
// /time joined it when the board learned to ask the network what time it is.
// It is here rather than behind a new system call because that is what this
// volume is for -- something the kernel knows, read as a file -- and `cat
// /var/time` needs no new module, no ABI number and no shell builtin.

#include "clock.h"
#include "pico/time.h"

uint32_t ubiqos_dmesg_size(void);
int32_t  ubiqos_dmesg_at(uint32_t offset);
void     ubiqos_print(const char *s);

static bool name_is(const char *path, const char *want) {
    for (int i = 0; want[i] || path[i]; i++)
        if (path[i] != want[i]) return false;
    return true;
}

static bool is_dmesg(const char *path) { return name_is(path, "/dmesg"); }
static bool is_time(const char *path)  { return name_is(path, "/time"); }
static bool is_utc(const char *path)   { return name_is(path, "/utc"); }
static bool is_uptime(const char *path) { return name_is(path, "/uptime"); }

// The line /var/time holds. Regenerated on every read, which is the point of
// it: two reads a minute apart differ.
static uint32_t time_line(char *out, uint32_t cap) {
    ubiqos_clock_stamp(out, cap);
    uint32_t n = 0;
    while (out[n]) n++;
    if (n + 2 <= cap) { out[n++] = '\n'; out[n] = 0; }
    return n;
}

// /utc is the same clock for a program rather than a person: seconds since
// 1970 in UTC, which is what a certificate's dates are compared with. Empty
// until the network has said what time it is -- an empty file cannot be
// mistaken for a date, and a zero could be.
static uint32_t utc_line(char *out, uint32_t cap) {
    if (!ubiqos_clock_is_set() || cap < 12) { if (cap) out[0] = 0; return 0; }
    uint32_t v = ubiqos_clock_utc(), n = 0;
    char tmp[11];
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n++] = '\n';
    out[n] = 0;
    return n;
}

// /uptime: seconds since reset, with milliseconds -- "65961.123" -- as Linux
// has /proc/uptime. From the hardware timer's 64 bits, so it neither wraps
// nor waits for the network, and the stamps in /var/dmesg count from the same
// moment: boot time plus a stamp is when a line was written.
static uint32_t uptime_line(char *out, uint32_t cap) {
    if (cap < 24) { if (cap) out[0] = 0; return 0; }
    const uint64_t ms = time_us_64() / 1000u;
    uint32_t sec = (uint32_t)(ms / 1000u), frac = (uint32_t)(ms % 1000u), n = 0;
    char tmp[10];
    do { tmp[n++] = (char)('0' + sec % 10u); sec /= 10u; } while (sec);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n++] = '.';
    out[n++] = (char)('0' + frac / 100u);
    out[n++] = (char)('0' + frac / 10u % 10u);
    out[n++] = (char)('0' + frac % 10u);
    out[n++] = '\n';
    out[n] = 0;
    return n;
}

// The three small files, one line each, made at the moment they are asked for.
static uint32_t small_line(const char *path, char *out, uint32_t cap) {
    if (is_utc(path))    return utc_line(out, cap);
    if (is_uptime(path)) return uptime_line(out, cap);
    return time_line(out, cap);
}

static int32_t var_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (is_time(path) || is_utc(path) || is_uptime(path)) {
        char line[24];
        const uint32_t n = small_line(path, line, sizeof line);
        if (offset >= n) return 0;
        uint32_t k = 0;
        while (k < len && offset + k < n) { buf[k] = (uint8_t)line[offset + k]; k++; }
        return (int32_t)k;
    }
    if (!is_dmesg(path)) return -1;
    uint32_t n = 0;
    while (n < len) {
        int32_t c = ubiqos_dmesg_at(offset + n);
        if (c < 0) break;                        // the end of what was said
        buf[n++] = (uint8_t)c;
    }
    return (int32_t)n;
}

static int32_t var_stat(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (path[0] == '/' && !path[1]) return UBIQOS_ATTR_DIRECTORY;
    if (is_time(path) || is_utc(path) || is_uptime(path)) {
        char line[24];
        if (size_out) *size_out = small_line(path, line, sizeof line);
        return 0;
    }
    if (!is_dmesg(path)) return -1;
    if (size_out) *size_out = ubiqos_dmesg_size();
    return 0;
}

static int32_t var_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (dirpath[0] != '/' || dirpath[1]) return -1;
    if (index > 3) return -1;
    static const char names[4][8] = { "dmesg", "time", "utc", "uptime" };
    const char *n = names[index];
    int i = 0;
    while (n[i]) { name_out[i] = n[i]; i++; }
    name_out[i] = 0;
    if (size_out) {
        char line[24];
        if (index == 3)      *size_out = uptime_line(line, sizeof line);
        else if (index == 2) *size_out = utc_line(line, sizeof line);
        else if (index == 1) *size_out = time_line(line, sizeof line);
        else                 *size_out = ubiqos_dmesg_size();
    }
    return 0;
}

static const ubiqos_fsops_t var_ops = {
    .read_at  = var_read_at,
    .stat_nth = var_stat_nth,
    .stat     = var_stat,
};

void ubiqos_varfs_init(void) {
    if (!ubiqos_vfs_add("var", &var_ops))
        ubiqos_print("var: no room in the volume table\n");
}
