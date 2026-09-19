#include "vfs.h"
#include "tlsf.h"
#include "../common/ubiqos_abi.h"

// /tmp -- files that live in PSRAM and go when the power does.
//
// Eight megabytes sit behind the second chip select doing very little, and a
// scratch file is exactly what bulk memory is for. The card is slow, wears out,
// and may not be there at all; this is none of those things.
//
// Flat: no directories, and eight files at once. A scratch filesystem that
// needed a directory tree would be a filesystem, and there is one of those.
//
// The immediate use is pipelines. A shell can put the left side's output here
// and give it to the right side, which is `a > /tmp/p` and `b < /tmp/p` -- and
// those two are the redirection that already works, rather than a second
// arrangement of descriptors that has to be got right again.

extern tlsf_pool_t ubiqos_bulk_pool;
extern tlsf_pool_t ubiqos_mem_pool;
void ubiqos_print(const char *s);

// Sixteen, up from eight. Eighty-four bytes an entry now that a name is a real
// name rather than twelve characters -- see the note on the name field.
#define TMP_MAX_FILES 16
#define TMP_GROW      1024        // rounded up to this, so appending is not a
                                  // reallocation per byte

typedef struct {
    // Room for a real filename, and it was twelve. That is FAT's old 8.3 limit
    // and nothing here is FAT -- but the consequence was worse than a short
    // name: create() truncated to eleven characters while find() compared the
    // whole name, so anything longer could never be found again and every open
    // with O_CREAT made another file. "/tmp/sensors.json" is twelve characters
    // and produced eight copies called "sensors.jso" before the table was full.
    char     name[UBIQOS_DIRNAME_MAX];
    uint8_t *data;
    uint32_t size;                // bytes written
    uint32_t cap;                 // bytes allocated
    bool     used;
} tmpfile_t;

static tmpfile_t files[TMP_MAX_FILES];

static bool name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// "/p" -> "p". Nothing here has a directory, so anything with a second slash is
// simply not found rather than being an error worth its own message.
static const char *leaf(const char *path) {
    if (!path || path[0] != '/') return 0;
    const char *p = path + 1;
    for (const char *q = p; *q; q++) if (*q == '/') return 0;
    return *p ? p : 0;
}

static tmpfile_t *find(const char *path) {
    const char *n = leaf(path);
    if (!n) return 0;
    for (int i = 0; i < TMP_MAX_FILES; i++)
        if (files[i].used && name_eq(files[i].name, n)) return &files[i];
    return 0;
}

static tmpfile_t *create(const char *path) {
    const char *n = leaf(path);
    if (!n) return 0;
    // Refused rather than cut, which is the other half of the fix above. A
    // truncated name is a name that does not match itself, and the file it
    // makes is one nobody can open -- see the same rule spelled out in
    // load_module_from_card, where cutting a name ran the wrong program.
    uint32_t len = 0;
    while (n[len]) len++;
    if (len >= UBIQOS_DIRNAME_MAX) return 0;

    for (int i = 0; i < TMP_MAX_FILES; i++) {
        if (files[i].used) continue;
        uint32_t k = 0;
        while (n[k]) { files[i].name[k] = n[k]; k++; }
        files[i].name[k] = 0;
        files[i].data = 0;
        files[i].size = files[i].cap = 0;
        files[i].used = true;
        return &files[i];
    }
    return 0;
}

// PSRAM if there is any, the SRAM pool if not: a machine without the second
// chip select should still have somewhere to put a scratch file, even a small
// one.
static tlsf_pool_t pool(void) {
    return ubiqos_bulk_pool ? ubiqos_bulk_pool : ubiqos_mem_pool;
}

static bool reserve(tmpfile_t *f, uint32_t want) {
    if (want <= f->cap) return true;
    uint32_t cap = (want + TMP_GROW - 1) / TMP_GROW * TMP_GROW;
    uint8_t *p = (uint8_t*)ubiqos_tlsf_malloc(pool(), cap);
    if (!p) return false;
    for (uint32_t i = 0; i < f->size; i++) p[i] = f->data[i];
    if (f->data) ubiqos_tlsf_free(pool(), f->data);
    f->data = p;
    f->cap = cap;
    return true;
}

static int32_t tmp_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len) {
    tmpfile_t *f = find(path);
    if (!f) return -1;
    if (offset >= f->size) return 0;                    // end of file
    if (len > f->size - offset) len = f->size - offset;
    for (uint32_t i = 0; i < len; i++) buf[i] = f->data[offset + i];
    return (int32_t)len;
}

static int32_t tmp_write_at(const char *path, uint32_t offset, const uint8_t *buf, uint32_t len) {
    tmpfile_t *f = find(path);
    if (!f) f = create(path);                           // writing brings it into being
    if (!f) return -1;
    if (!reserve(f, offset + len)) return -1;
    // A gap between the end and the offset is zeroed rather than left as
    // whatever PSRAM held, which is somebody else's data.
    for (uint32_t i = f->size; i < offset; i++) f->data[i] = 0;
    for (uint32_t i = 0; i < len; i++) f->data[offset + i] = buf[i];
    if (offset + len > f->size) f->size = offset + len;
    return (int32_t)len;
}

static bool tmp_remove(const char *path) {
    tmpfile_t *f = find(path);
    if (!f) return false;
    if (f->data) ubiqos_tlsf_free(pool(), f->data);
    f->data = 0;
    f->size = f->cap = 0;
    f->used = false;
    return true;
}

// A new name, replacing whatever already had it, as rename(2) does. That is
// what makes it the way to publish a file somebody else reads: write it under
// another name, then rename it over the old one. The filesystem server takes
// one message at a time, so a reader sees the old file or the new one and
// never the moment between -- which a rewrite in place cannot promise, and
// O_TRUNC even less, since it removes the file first. hibouair's sensors.json
// is why this exists.
static bool tmp_rename(const char *from, const char *to) {
    tmpfile_t *f = find(from);
    const char *n = leaf(to);
    if (!f || !n) return false;
    uint32_t len = 0;
    while (n[len]) len++;
    if (len >= UBIQOS_DIRNAME_MAX) return false;
    tmpfile_t *old = find(to);
    if (old == f) return true;
    if (old) tmp_remove(to);
    for (uint32_t k = 0; k <= len; k++) f->name[k] = n[k];
    return true;
}

static int32_t tmp_stat(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (path && path[0] == '/' && !path[1]) return UBIQOS_ATTR_DIRECTORY;
    tmpfile_t *f = find(path);
    if (!f) return -1;
    if (size_out) *size_out = f->size;
    return 0;
}

static int32_t tmp_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (!dirpath || dirpath[0] != '/' || dirpath[1]) return -1;   // no subdirectories
    uint32_t seen = 0;
    for (int i = 0; i < TMP_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (seen++ != index) continue;
        uint32_t k = 0;
        while (files[i].name[k]) { name_out[k] = files[i].name[k]; k++; }
        name_out[k] = 0;
        if (size_out) *size_out = files[i].size;
        return 0;
    }
    return -1;
}

const ubiqos_fsops_t ubiqos_tmpfs_ops = {
    .read_at  = tmp_read_at,
    .write_at = tmp_write_at,
    .remove   = tmp_remove,
    .rename   = tmp_rename,
    .stat_nth = tmp_stat_nth,
    .stat     = tmp_stat,
};

// No find_nth, so ubiqos_vfs_module_volume passes it over: nobody should be
// looking for modules in scratch space.
void ubiqos_tmpfs_init(void) {
    for (int i = 0; i < TMP_MAX_FILES; i++) files[i].used = false;
    if (!ubiqos_vfs_add("tmp", &ubiqos_tmpfs_ops))
        ubiqos_print("tmp: no room in the volume table\n");
}
