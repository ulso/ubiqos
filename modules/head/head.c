#include "../../common/myrtos_stdio.h"

// head -- the first few lines of a file.
//
//     head notes.txt
//     head /sd/docs/readme.txt
//     head /sd/big.txt 4096      -- from that byte onwards
//
// Written against myrtos_stdio.h, so it is also the answer to "can ordinary C
// be built here": fopen, fgets, fseek, ftell, ferror and fclose, with nothing
// myrtos-shaped in it but the include and the entry point's name.

// The one line a C library would have owed us: errno and the stream table.
MYRTOS_STDIO_DEFINE

// Stdio's buffer comes from PSRAM, but the line buffer here is on the stack,
// and eight kilobytes is what makes room for both.
MYRTOS_MEM_SIZE(8192);

#define LINES 10

static bool parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0, n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
    *out = v;
    return n && !*s;
}

void module_main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: head FILE [SKIP]\n", stderr);
        return;
    }

    uint32_t skip = 0;
    if (argc >= 3 && !parse_u32(argv[2], &skip)) {
        fputs("head: SKIP must be a number\n", stderr);
        return;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) { fputs("head: cannot open it\n", stderr); return; }

    if (skip && fseek(f, (int32_t)skip, SEEK_SET) < 0) {
        fputs("head: cannot seek there\n", stderr);
        fclose(f);
        return;
    }

    char line[128];
    int n = 0;
    while (n < LINES && fgets(line, (int)sizeof line, f)) { fputs(line, stdout); n++; }

    // Opening does not check that a file exists -- there are no flags yet to
    // say whether a write should create -- so an empty first read is where a
    // missing file turns up, and ferror is what tells it from a real end.
    if (!n && ferror(f)) fputs("head: no such file\n", stderr);

    if (skip) {
        char msg[32];
        int32_t at = ftell(f);
        int i = 0;
        for (uint32_t v = (uint32_t)at, d = 1000000000u; d; d /= 10)
            if (v / d || i || d == 1) { msg[i++] = (char)('0' + (v / d) % 10); }
        msg[i] = 0;
        fputs("head: stopped at ", stderr);
        fputs(msg, stderr);
        fputs("\n", stderr);
    }

    fclose(f);
}
