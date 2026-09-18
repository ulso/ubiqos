#include "../../common/ubiqos_abi.h"

// cp -- copies a file on the card, streaming in pieces so the size of the file
// is not bounded by the 4 kB a process gets for data and stack.

#define CHUNK 256

static void fail(const char *what, const char *name) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "cp: ");
    ubiqos_line_str(&l, name);
    ubiqos_line_str(&l, ": ");
    ubiqos_line_str(&l, what);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDERR, &l);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: cp SOURCE DEST\n")) return;

    if (argc != 3) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: cp SOURCE DEST\n");
        return;
    }
    const char *src = argv[1], *dst = argv[2];

    // The destination goes first. Writing does not truncate, so copying a short
    // file over a long one would otherwise leave the old tail in place.
    ubiqos_fs_remove(dst);

    uint8_t buf[CHUNK];
    uint32_t offset = 0;
    for (;;) {
        int32_t n = ubiqos_fs_read(src, offset, buf, CHUNK);
        if (n < 0) { fail("no such file", src); return; }
        if (n == 0) break;
        if (ubiqos_fs_write(dst, offset, buf, (uint32_t)n) != n) {
            fail("write failed", dst);
            return;
        }
        offset += (uint32_t)n;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_u32(&l, offset);
    ubiqos_line_str(&l, " bytes copied\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}
