#ifndef MYRTOS_VFS_H
#define MYRTOS_VFS_H

#include <stdint.h>
#include <stdbool.h>

// Volumes, and the root that lists them.
//
// A path begins with a volume name -- "/sd/docs/readme.txt" -- and the
// filesystem server strips that first component off and hands the rest to
// whoever owns it. This is OS-9's arrangement, where "/d0" and "/h0" sat at the
// top and the I/O manager dispatched on the name in front. It is also what
// io.c already does for character devices, one storey down: a name, a table,
// and a struct of function pointers. This is the same shape.
//
// The root itself is owned by nobody. Listing it lists the volumes, which is
// why it is answered here rather than by any filesystem.

// Everything the server asks of a volume. A volume that cannot do something
// leaves the pointer null and the request is refused -- /dev has only stat_nth,
// because a device is not a file you can write to by name.
//
// The signatures are FAT32's unchanged, so that filesystem needs no wrappers.
typedef struct {
    int32_t (*read_at)(const char *path, uint32_t offset, uint8_t *buf, uint32_t len);
    int32_t (*write_at)(const char *path, uint32_t offset, const uint8_t *buf, uint32_t len);
    bool    (*remove)(const char *path);
    bool    (*mkdir)(const char *path);
    bool    (*rmdir)(const char *path);
    int32_t (*stat_nth)(const char *dirpath, uint32_t index, char *name_out, uint32_t *size_out);
    // Module scanning: find the nth file with this extension, then read it.
    bool    (*find_nth)(const char *ext_3, uint32_t index, char *name_out);
    int32_t (*read_file)(const char *name_83, uint8_t *buf, uint32_t max_len);
} myrtos_fsops_t;

#define MYRTOS_MAX_VOLUMES 4

void myrtos_vfs_init(void);

// Name it, or replace what is there under the same name -- remounting a card
// is the same volume with new contents. False when the table is full.
bool myrtos_vfs_add(const char *name, const myrtos_fsops_t *ops);
void myrtos_vfs_remove(const char *name);

// Split an absolute path into its volume and the rest. "/sd/docs/x" gives "sd"
// and "/docs/x"; "/sd" gives "sd" and "/"; "/" gives no volume at all.
// Returns the ops, or null when the volume is not mounted or the path is the
// root. rest_out points into path.
const myrtos_fsops_t *myrtos_vfs_split(const char *path, const char **rest_out);

// The root directory: one entry per mounted volume, in the same form
// stat_nth gives. Returns the attribute byte or -1 past the end.
int32_t myrtos_vfs_root_nth(uint32_t index, char *name_out, uint32_t *size_out);

// The volume that holds modules to register, or null when none is mounted.
const myrtos_fsops_t *myrtos_vfs_module_volume(const char **name_out);

#endif
