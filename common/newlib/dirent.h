// <dirent.h> for a module built NEWLIB.
//
// Newlib's own is two lines and one of them is #error "<dirent.h> not
// supported" -- reading a directory is not a C library matter, it is a system
// one, and a library built for no system in particular cannot have an opinion.
// So UbiqOS supplies it, and this header comes first on the include path.
//
// The shape is POSIX's because that is what ported code was written against.
// What is underneath is not a handle but an index: UbiqOS answers "the nth
// entry of this directory", which is the same question readdir asks and the
// reason this is thirty lines rather than a directory cache.
#ifndef UBIQOS_DIRENT_H
#define UBIQOS_DIRENT_H

#include <stdint.h>
#include "ubiqos_abi.h"

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
    uint32_t d_ino;                     // never zero; see the note in the .c
    uint32_t d_type;                    // DT_REG or DT_DIR
    uint32_t d_size;                    // not POSIX, and free to answer here
    char     d_name[UBIQOS_DIRNAME_MAX];
};

typedef struct {
    char           path[256];
    uint32_t       index;
    struct dirent  entry;               // handed back by pointer, as readdir does
} DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);

#endif
