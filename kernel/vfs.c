#include "vfs.h"
#include "io.h"
#include "../common/ubiqos_abi.h"

// See vfs.h for the shape of this. The table is four entries because a machine
// with more than a handful of volumes is not the machine this is for, and a
// fixed table needs no allocator on a path that must work before one exists.

typedef struct {
    char name[12];
    const ubiqos_fsops_t *ops;
} volume_t;

static volume_t volumes[UBIQOS_MAX_VOLUMES];
static uint32_t volume_count;

static bool name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *dst, const char *src) {
    uint32_t i = 0;
    while (src[i] && i < 11) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// --- /dev -----------------------------------------------------------------
// The device table io.c already keeps, shown as a directory. Only listing is
// implemented, and that is not a gap: a device is opened by name through the
// I/O manager, not read as a file. /dev is here so the root is never empty --
// a machine with no card should still have something to look at.
static int32_t dev_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (dirpath[0] != '/' || dirpath[1] != 0) return -1;   // no subdirectories
    if (!ubiqos_io_device_nth(index, name_out)) return -1;
    if (size_out) *size_out = 0;
    return 0;                                              // a file, not a directory
}

// A device has no length, but it does have existence, and that is the half of
// stat anyone asks /dev about.
static int32_t dev_stat(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (!path || path[0] != '/') return -1;
    if (!path[1]) return UBIQOS_ATTR_DIRECTORY;          // /dev itself
    return ubiqos_io_has_device(path + 1) ? 0 : -1;
}

static const ubiqos_fsops_t dev_ops = {
    .stat_nth = dev_stat_nth,
    .stat     = dev_stat,
};

void ubiqos_vfs_init(void) {
    volume_count = 0;
    ubiqos_vfs_add("dev", &dev_ops);
}

bool ubiqos_vfs_add(const char *name, const ubiqos_fsops_t *ops) {
    for (uint32_t i = 0; i < volume_count; i++) {
        if (name_eq(volumes[i].name, name)) {       // a remount is the same volume
            volumes[i].ops = ops;
            return true;
        }
    }
    if (volume_count >= UBIQOS_MAX_VOLUMES) return false;
    name_copy(volumes[volume_count].name, name);
    volumes[volume_count].ops = ops;
    volume_count++;
    return true;
}

void ubiqos_vfs_remove(const char *name) {
    for (uint32_t i = 0; i < volume_count; i++) {
        if (!name_eq(volumes[i].name, name)) continue;
        volumes[i] = volumes[--volume_count];        // close the gap
        return;
    }
}

const ubiqos_fsops_t *ubiqos_vfs_split(const char *path, const char **rest_out) {
    if (!path || path[0] != '/') return 0;

    char vol[12];
    uint32_t n = 0;
    uint32_t i = 1;
    while (path[i] && path[i] != '/' && n < sizeof(vol) - 1) vol[n++] = path[i++];
    vol[n] = 0;
    if (!n) return 0;                                // the root itself

    // What follows is a path in that volume's own terms, and it must start with
    // a slash: "/sd" means that volume's root, not an empty string.
    *rest_out = path[i] ? &path[i] : "/";

    for (uint32_t k = 0; k < volume_count; k++)
        if (name_eq(volumes[k].name, vol)) return volumes[k].ops;
    return 0;
}

int32_t ubiqos_vfs_root_nth(uint32_t index, char *name_out, uint32_t *size_out) {
    if (index >= volume_count) return -1;
    name_copy(name_out, volumes[index].name);
    if (size_out) *size_out = 0;
    return UBIQOS_ATTR_DIRECTORY;
}

