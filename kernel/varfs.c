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

// The line /var/time holds. Regenerated on every read, which is the point of
// it: two reads a minute apart differ.
static uint32_t time_line(char *out, uint32_t cap) {
    ubiqos_clock_stamp(out, cap);
    uint32_t n = 0;
    while (out[n]) n++;
    if (n + 2 <= cap) { out[n++] = '\n'; out[n] = 0; }
    return n;
}

static int32_t var_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (is_time(path)) {
        char line[24];
        const uint32_t n = time_line(line, sizeof line);
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
    if (is_time(path)) {
        char line[24];
        if (size_out) *size_out = time_line(line, sizeof line);
        return 0;
    }
    if (!is_dmesg(path)) return -1;
    if (size_out) *size_out = ubiqos_dmesg_size();
    return 0;
}

static int32_t var_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (dirpath[0] != '/' || dirpath[1]) return -1;
    if (index > 1) return -1;
    const char *n = index ? "time" : "dmesg";
    int i = 0;
    while (n[i]) { name_out[i] = n[i]; i++; }
    name_out[i] = 0;
    if (size_out) {
        if (index) { char line[24]; *size_out = time_line(line, sizeof line); }
        else       { *size_out = ubiqos_dmesg_size(); }
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
