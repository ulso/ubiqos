#ifndef MYRTOS_FAT32_H
#define MYRTOS_FAT32_H

#include <stdint.h>
#include <stdbool.h>

// FAT32 with one partition and short 8.3 names. Reading, writing and
// subdirectories; long names do not exist. Paths are absolute and separated by
// slashes -- "/docs/readme.txt" -- and an empty path is the root.

bool     myrtos_fat_mount(void);
int32_t  myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len);

// Enumerate files in the root directory with a given extension. index starts
// at zero; returns false when there are no more. name_out is filled with eleven
// characters plus a NUL, the raw 8.3 form myrtos_fat_read_file takes.
bool     myrtos_fat_find_nth(const char *ext_3, uint32_t index, char *name_out);

// Read len bytes starting at offset. Returns bytes read, 0 at end of file.
int32_t  myrtos_fat_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len);

// Enumerate every visible entry in one directory, unfiltered. Returns the
// attribute byte, or -1 when there are no more. name_out takes twelve bytes.
int32_t  myrtos_fat_stat_nth(const char *dirpath, uint32_t index,
                             char *name_out, uint32_t *size_out);

// Write a slice of a file, creating and extending it as needed.
int32_t  myrtos_fat_write_at(const char *path, uint32_t offset,
                             const uint8_t *buf, uint32_t len);

// Delete a file. Refuses directories.
bool     myrtos_fat_remove(const char *path);

// Make a directory, with its "." and ".." in place before it is named.
bool     myrtos_fat_mkdir(const char *path);

// Remove a directory. Refuses one that still has anything in it.
bool     myrtos_fat_rmdir(const char *path);

// One named entry: its attribute byte, or -1 when there is no such thing. The
// size comes back through the pointer, and a directory reports zero.
int32_t  myrtos_fat_stat(const char *path, uint32_t *size_out);

// Take the card again from the beginning, for one swapped while running.
bool     myrtos_fat_remount(void);

// "readme.txt" -> "README  TXT", the form the directory actually stores.
bool     myrtos_fat_name_to_83(const char *user, char *out_11);

// The same functions as a volume, for the filesystem server to mount by name.
// The signatures above were not changed to fit: the struct was shaped to them.
#include "vfs.h"
extern const myrtos_fsops_t myrtos_fat_ops;

#endif
