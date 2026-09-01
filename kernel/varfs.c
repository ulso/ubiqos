#include "vfs.h"
#include "../common/myrtos_abi.h"

// /var -- what the kernel has said, as a file.
//
// One file and read only. It exists because the boot messages go to the screen
// and the UART, and a session on the USB console never sees them: by the time
// that console exists the kernel has already said everything interesting. This
// is where to read it afterwards.
//
// The ring lives in main.c beside myrtos_print, because the first line is
// written before any of this is running.

uint32_t myrtos_dmesg_size(void);
int32_t  myrtos_dmesg_at(uint32_t offset);
void     myrtos_print(const char *s);

static bool is_dmesg(const char *path) {
    static const char want[] = "/dmesg";
    for (int i = 0; want[i] || path[i]; i++)
        if (path[i] != want[i]) return false;
    return true;
}

static int32_t var_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (!is_dmesg(path)) return -1;
    uint32_t n = 0;
    while (n < len) {
        int32_t c = myrtos_dmesg_at(offset + n);
        if (c < 0) break;                        // the end of what was said
        buf[n++] = (uint8_t)c;
    }
    return (int32_t)n;
}

static int32_t var_stat(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (path[0] == '/' && !path[1]) return MYRTOS_ATTR_DIRECTORY;
    if (!is_dmesg(path)) return -1;
    if (size_out) *size_out = myrtos_dmesg_size();
    return 0;
}

static int32_t var_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (dirpath[0] != '/' || dirpath[1]) return -1;
    if (index) return -1;                        // there is only the one
    const char *n = "dmesg";
    int i = 0;
    while (n[i]) { name_out[i] = n[i]; i++; }
    name_out[i] = 0;
    if (size_out) *size_out = myrtos_dmesg_size();
    return 0;
}

static const myrtos_fsops_t var_ops = {
    .read_at  = var_read_at,
    .stat_nth = var_stat_nth,
    .stat     = var_stat,
};

void myrtos_varfs_init(void) {
    if (!myrtos_vfs_add("var", &var_ops))
        myrtos_print("var: no room in the volume table\n");
}
