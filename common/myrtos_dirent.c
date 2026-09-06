// Reading a directory, for a module built NEWLIB.
//
// Newlib has no answer for this and says so: its <dirent.h> is an #error. That
// is the right answer for a library built for no system in particular, and it
// is the one thing missing that most often stops a port dead -- anything that
// walks a tree, completes a filename or looks for a config file opens a
// directory on its first page.
//
// Underneath is not a handle but an index. myrtos answers "the nth entry of
// this directory", which is the same question readdir asks, so a DIR is a path
// and a counter and there is no cache to keep coherent. A file created while
// somebody is reading may be seen or missed depending on where the index has
// got to, which is what POSIX allows and what a real readdir does anyway.
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include "myrtos_abi.h"

DIR *opendir(const char *path)
{
    if (!path) { errno = EINVAL; return 0; }

    // Asked once, so that a directory that is not there fails here rather than
    // by returning no entries -- which a caller cannot tell from an empty one.
    // ls learned this the hard way and the note beside it says so.
    uint32_t size = 0;
    char probe[MYRTOS_DIRNAME_MAX];
    if (myrtos_fs_dir_at(path, 0, probe, &size) < 0) {
        uint32_t st = 0;
        int32_t attr = myrtos_fs_stat(path, &st);
        if (attr < 0 || !(attr & MYRTOS_ATTR_DIRECTORY)) {
            errno = ENOENT;
            return 0;
        }
        // It exists and is a directory; it is simply empty.
    }

    DIR *d = (DIR *)malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return 0; }

    unsigned n = 0;
    while (path[n] && n < sizeof d->path - 1) { d->path[n] = path[n]; n++; }
    d->path[n] = 0;
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (!d) { errno = EBADF; return 0; }

    // The kernel hands back what FAT stores -- "W4         ", eleven characters
    // with the extension implied by position -- and a caller needs a name it
    // can pass straight to open(). The rule is in myrtos_abi.h because ls and
    // the wasm host need the same one.
    uint32_t size = 0;
    char raw[MYRTOS_DIRNAME_MAX];
    int32_t attr = myrtos_fs_dir_at(d->path, d->index, raw, &size);
    if (attr < 0) return 0;                       // the end, and not an error
    d->index++;
    myrtos_pretty_name(raw, d->entry.d_name);

    // Never zero. POSIX does not say what an inode number means here and myrtos
    // has none to give, but a zero d_ino is treated as "no entry" by enough
    // code to be worth avoiding -- wasi-libc drops the entry outright and that
    // cost an evening on the guest side. An FNV hash of the name is stable
    // within a directory, which is as much as anything actually relies on.
    uint32_t h = 2166136261u;
    for (const char *p = d->entry.d_name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    d->entry.d_ino  = h | 1u;
    d->entry.d_type = (attr & MYRTOS_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
    d->entry.d_size = size;
    return &d->entry;
}

void rewinddir(DIR *d) { if (d) d->index = 0; }

int closedir(DIR *d)
{
    if (!d) { errno = EBADF; return -1; }
    free(d);
    return 0;
}
