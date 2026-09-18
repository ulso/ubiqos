// The kernel's side of the FAT32 library.
//
// fat32 was 9.3 kB of kernel text that only mattered once somebody touched the
// card. It is a LIBRARY module now -- modules/fat32lib/fat32lib.c -- and this
// keeps the four names the rest of the kernel calls, so fsserver.c and usbmsc.c
// do not know the difference.
//
// Linked on the first call, which is the mount. A board with no fat32lib in
// flash has no card and says nothing, exactly as one with no wifilib has no
// wifi.
#include <stdint.h>
#include <stdbool.h>
#include "../common/ubiqos_abi.h"
#include "moddir.h"

void ubiqos_print(const char *s);
extern const ubiqos_kernel_api_t ubiqos_kernel_api;

enum { FAT_INIT = 0, FAT_MOUNT = 1, FAT_OPS = 2, FAT_STAT = 3, FAT_EXTENT = 4 };

static const ubiqos_lib_table_t *lib;

static bool ensure_linked(void)
{
    if (lib) return true;
    lib = ubiqos_lib_link("fat32lib", 0);
    if (!lib || lib->count <= FAT_EXTENT) { lib = 0; return false; }

    bool (*init)(const ubiqos_kernel_api_t *) =
        (bool (*)(const ubiqos_kernel_api_t *))lib->fn[FAT_INIT];
    if (!init(&ubiqos_kernel_api)) { lib = 0; return false; }

    ubiqos_print("fat32: library linked, running from the module pool\n");
    return true;
}

bool ubiqos_fat_mount(void)
{
    if (!ensure_linked()) return false;
    return ((bool (*)(void))lib->fn[FAT_MOUNT])();
}

int32_t ubiqos_fat_stat(const char *path, uint32_t *size_out)
{
    if (!lib) return -1;
    return ((int32_t (*)(const char *, uint32_t *))lib->fn[FAT_STAT])(path, size_out);
}

bool ubiqos_fat_extent(uint32_t *first_block, uint32_t *block_count)
{
    if (!lib) return false;
    return ((bool (*)(uint32_t *, uint32_t *))lib->fn[FAT_EXTENT])(first_block, block_count);
}

// The operation table, which the file server hands to ubiqos_vfs_add. A pointer
// into the relocated copy in PSRAM, valid for as long as the library is linked
// -- which is for ever, since nothing unlinks it.
const ubiqos_fsops_t *ubiqos_fat_ops_ptr(void)
{
    if (!ensure_linked()) return 0;
    return (const ubiqos_fsops_t *)lib->fn[FAT_OPS];
}
