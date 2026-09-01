#include "../../common/myrtos_abi.h"

// head -- the first few lines of a file.
//
//     head notes.txt
//     head /sd/docs/readme.txt
//     head /sd/big.txt 4096      -- from that byte onwards
//
// Also the smallest complete example of opening, seeking, reading and closing.

#define LINES 10
#define CHUNK 128

static bool parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0, n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
    *out = v;
    return n && !*s;
}

void module_main(int argc, char **argv) {
    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDERR, "usage: head FILE [SKIP]\n");
        return;
    }

    uint32_t skip = 0;
    if (argc >= 3 && !parse_u32(argv[2], &skip)) {
        myrtos_write_str(MYRTOS_STDERR, "head: SKIP must be a number\n");
        return;
    }

    // The buffer is a local. A module may not have writable statics -- one copy
    // of the code serves every process running it, so they would share it.
    uint8_t buf[CHUNK];

    int32_t fd = myrtos_open(argv[1]);
    if (fd < 0) {
        myrtos_write_str(MYRTOS_STDERR, "head: cannot open it\n");
        return;
    }

    if (skip && myrtos_seek(fd, (int32_t)skip, MYRTOS_SEEK_SET) < 0) {
        myrtos_write_str(MYRTOS_STDERR, "head: cannot seek there\n");
        myrtos_close(fd);
        return;
    }

    int lines = 0;
    for (;;) {
        int32_t n = myrtos_read(fd, buf, CHUNK);
        // Nought is the end of the file; a negative is a file that was never
        // there, because opening does not check.
        if (n < 0) { myrtos_write_str(MYRTOS_STDERR, "head: no such file\n"); break; }
        if (n == 0) break;

        // Stop on the tenth newline rather than after ten chunks.
        uint32_t take = 0;
        while (take < (uint32_t)n && lines < LINES) {
            if (buf[take] == '\n') lines++;
            take++;
        }
        myrtos_write(MYRTOS_STDOUT, buf, take);
        if (lines >= LINES) break;
    }

    // Where it stopped, which is what ftell is for -- and only when a skip was
    // asked for, because whoever counts bytes in is the one who wants to know
    // where they came out.
    if (skip) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "head: stopped at ");
        myrtos_line_u32(&l, (uint32_t)myrtos_tell(fd));
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDERR, &l);
    }

    myrtos_close(fd);
}
