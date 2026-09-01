#include "../../common/myrtos_abi.h"

// cat -- writes files to standard output.
//
// The file is read a piece at a time rather than whole. A process gets 4 kB for
// data and stack together, so holding a file in memory would put an arbitrary
// ceiling on what cat can show; reading in slices puts none.
//
// Through a descriptor rather than a path and an offset, which is what the
// system had before descriptors reached files. The position lives in the
// descriptor now, so the loop no longer counts bytes -- and this is the shape
// fopen and fread will want underneath them.

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
        int32_t fd = myrtos_open(argv[i]);
        if (fd < 0) { complain(argv[i]); continue; }
        for (;;) {
            int32_t n = myrtos_read(fd, buf, CHUNK);
            // Nought is the end of the file and a negative is a file that was
            // never there. Opening does not check -- it cannot, while there are
            // no flags to say whether a write should create -- so this is where
            // a missing file is discovered, and collapsing the two into one
            // test made cat silent about it.
            if (n < 0) { complain(argv[i]); break; }
            if (n == 0) break;
            myrtos_write(MYRTOS_STDOUT, buf, (uint32_t)n);
        }
        myrtos_close(fd);
    }
}
