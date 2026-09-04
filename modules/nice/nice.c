#include "../../common/myrtos_abi.h"

// nice -- runs a command at a given priority.
//
//     nice 25 fill big.txt 100000
//
// It needs no help from the kernel beyond what already exists: a child inherits
// its parent's priority, so nice sets its own and starts the command. The
// program being run knows nothing about any of it.

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

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: nice PRIORITY COMMAND [ARGS...]\n\nRuns a command at another priority. 0 is idle, 31 the most urgent.\n")) return;

    uint32_t prio;
    if (argc < 3 || !parse_u32(argv[1], &prio)) {
        myrtos_write_str(MYRTOS_STDERR, "usage: nice PRIORITY COMMAND [ARGS...]\n");
        return;
    }

    // argv was split in place, so the words are separate strings now. Putting
    // the tail back together is cheaper than teaching exec about vectors.
    char args[80];
    uint32_t n = 0;
    for (int i = 3; i < argc; i++) {
        if (n && n < sizeof(args) - 1) args[n++] = ' ';
        for (const char *p = argv[i]; *p && n < sizeof(args) - 1; p++) args[n++] = *p;
    }
    args[n] = 0;

    myrtos_setprio(prio);

    int32_t pid = myrtos_exec(argv[2], args);
    if (pid < 0) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "nice: ");
        myrtos_line_str(&l, argv[2]);
        myrtos_line_str(&l, ": no such module\n");
        myrtos_line_flush(MYRTOS_STDERR, &l);
        return;
    }
    myrtos_wait(pid);
}
