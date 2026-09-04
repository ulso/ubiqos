#include "../../common/myrtos_abi.h"

// cp -- copies a file on the card, streaming in pieces so the size of the file
// is not bounded by the 4 kB a process gets for data and stack.

#define CHUNK 256

static void fail(const char *what, const char *name) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "cp: ");
    myrtos_line_str(&l, name);
    myrtos_line_str(&l, ": ");
    myrtos_line_str(&l, what);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDERR, &l);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: cp SOURCE DEST\n")) return;

    if (argc != 3) {
        myrtos_write_str(MYRTOS_STDERR, "usage: cp SOURCE DEST\n");
        return;
    }
    const char *src = argv[1], *dst = argv[2];

    // The destination goes first. Writing does not truncate, so copying a short
    // file over a long one would otherwise leave the old tail in place.
    myrtos_fs_remove(dst);

    uint8_t buf[CHUNK];
    uint32_t offset = 0;
    for (;;) {
        int32_t n = myrtos_fs_read(src, offset, buf, CHUNK);
        if (n < 0) { fail("no such file", src); return; }
        if (n == 0) break;
        if (myrtos_fs_write(dst, offset, buf, (uint32_t)n) != n) {
            fail("write failed", dst);
            return;
        }
        offset += (uint32_t)n;
    }

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_u32(&l, offset);
    myrtos_line_str(&l, " bytes copied\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
