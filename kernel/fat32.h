#ifndef MYRTOS_FAT32_H
#define MYRTOS_FAT32_H

#include <stdint.h>
#include <stdbool.h>

// Read-only FAT32, enough to fetch modules off the card: one partition, short
// 8.3 names, the root directory. Writing, long names and subdirectories do not
// exist, and should be added when something needs them.

bool     myrtos_fat_mount(void);
int32_t  myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len);

// Enumerate files in the root directory with a given extension. index starts
// at zero; returns false when there are no more. name_out is filled with eleven
// characters plus a NUL, the raw 8.3 form myrtos_fat_read_file takes.
bool     myrtos_fat_find_nth(const char *ext_3, uint32_t index, char *name_out);

#endif
