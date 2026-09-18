#include "../../common/ubiqos_abi.h"

// fill -- writes a known pattern to a file and reads it back to check it.
//
//     fill big.txt 100000
//
// Small files never leave the first sector of the first cluster, so the parts
// of the write path most likely to be wrong -- crossing a sector, extending a
// cluster chain -- go untested by ordinary use. This writes past those
// boundaries deliberately and says where the first byte went wrong.

#define CHUNK 512

// Printable and position-dependent, so a displaced byte is visible in cat as
// well as caught here. The newline every 64 bytes is only to keep cat readable.
static uint8_t pattern(uint32_t i) {
    return (i % 64 == 63) ? (uint8_t)'\n' : (uint8_t)('A' + (i % 26));
}

static bool parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

static void say(const char *a, const char *b, uint32_t n, bool with_n) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, a);
    if (b) ubiqos_line_str(&l, b);
    if (with_n) ubiqos_line_u32(&l, n);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: fill FILE BYTES\n\nWrites BYTES of test pattern.\n")) return;

    uint32_t total;
    if (argc != 3 || !parse_u32(argv[2], &total) || !total) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: fill FILE BYTES\n");
        return;
    }

    ubiqos_fs_remove(argv[1]);

    uint8_t buf[CHUNK];
    uint32_t offset = 0;
    while (offset < total) {
        uint32_t n = total - offset;
        if (n > CHUNK) n = CHUNK;
        for (uint32_t j = 0; j < n; j++) buf[j] = pattern(offset + j);
        if (ubiqos_fs_write(argv[1], offset, buf, n) != (int32_t)n) {
            say("fill: write failed at ", 0, offset, true);
            return;
        }
        offset += n;
    }
    say("wrote ", 0, total, true);

    // Read it back through the same path a reader would use, so the check
    // exercises the chain walk as well as the write.
    offset = 0;
    while (offset < total) {
        int32_t n = ubiqos_fs_read(argv[1], offset, buf, CHUNK);
        if (n <= 0) { say("fill: read stopped at ", 0, offset, true); return; }
        for (int32_t j = 0; j < n; j++) {
            if (buf[j] == pattern(offset + (uint32_t)j)) continue;
            say("fill: MISMATCH at ", 0, offset + (uint32_t)j, true);
            return;
        }
        offset += (uint32_t)n;
    }

    if (offset != total) { say("fill: short read, got ", 0, offset, true); return; }
    say("verified ", 0, total, true);
}
