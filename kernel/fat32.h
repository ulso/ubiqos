#ifndef MYRTOS_FAT32_H
#define MYRTOS_FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"   // myrtos_fsops_t lives there now

// FAT32 with one partition and short 8.3 names. Reading, writing and
// subdirectories; long names do not exist. Paths are absolute and separated by
// slashes -- "/docs/readme.txt" -- and an empty path is the root.

// What the kernel still calls. Everything else fat32 can do is reached through
// the operation table, which the file server registers with the vfs -- see
// kernel/fat32link.c, and modules/fat32lib for the code itself.
bool     myrtos_fat_mount(void);
bool     myrtos_fat_extent(uint32_t *first_block, uint32_t *block_count);
int32_t  myrtos_fat_stat(const char *path, uint32_t *size_out);
const myrtos_fsops_t *myrtos_fat_ops_ptr(void);

#endif
