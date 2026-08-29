#include "../../common/myrtos_abi.h"

// cat -- writes files to standard output.
//
// The file is read a piece at a time rather than whole. A process gets 4 kB for
// data and stack together, so holding a file in memory would put an arbitrary
// ceiling on what cat can show; reading in slices puts none.

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
        uint32_t offset = 0;
        for (;;) {
            int32_t n = myrtos_fs_read(argv[i], offset, buf, CHUNK);
            if (n < 0) { complain(argv[i]); break; }
            if (n == 0) break;                  // end of file
            myrtos_write(MYRTOS_STDOUT, buf, (uint32_t)n);
            offset += (uint32_t)n;
        }
    }
}
