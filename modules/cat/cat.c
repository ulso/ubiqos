#include "../../common/myrtos_posix.h"

// The one definition of errno the program owes, exactly as a C library would
// have provided it. Thread-local because it is per-process writable state, and
// a shareable module may not have that as a static.
__thread int errno;

// cat -- writes files to standard output.
//
// The file is read a piece at a time rather than whole. A process gets 4 kB for
// data and stack together, so holding a file in memory would put an arbitrary
// ceiling on what cat can show; reading in slices puts none.
//
// Written against myrtos_posix.h rather than the system calls, so this file is
// also the answer to "can ordinary C be built here": open, read, close and
// STDOUT_FILENO, with nothing myrtos-shaped in the loop at all.

#define CHUNK 256

static void complain(const char *name) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "cat: ");
    myrtos_line_str(&l, name);
    myrtos_line_str(&l, ": no such file\n");
    myrtos_line_flush(MYRTOS_STDERR, &l);
}

void module_main(int argc, char **argv) {
    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDERR, "usage: cat FILE...\n");
        return;
    }

    uint8_t buf[CHUNK];
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { complain(argv[i]); continue; }
        for (;;) {
            int32_t n = read(fd, buf, CHUNK);
            // Nought is the end of the file and a negative is a file that was
            // never there. Opening does not check -- it cannot, while there are
            // no flags to say whether a write should create -- so this is where
            // a missing file is discovered, and collapsing the two into one
            // test made cat silent about it.
            if (n < 0) { complain(argv[i]); break; }
            if (n == 0) break;
            write(STDOUT_FILENO, buf, (uint32_t)n);
        }
        close(fd);
    }
}
