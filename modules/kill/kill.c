#include "../../common/myrtos_abi.h"

// kill -- end a process by number.
//
// The kernel's own service threads are refused. The machine needs the console,
// the filesystem and the radio, and nobody chose to start them, so nobody
// should be able to end them by mistyping a number.
//
// A process blocked on one of those servers does not disappear at once: the
// server holds a pointer into its memory, so it stops running immediately and
// is taken apart when the reply arrives. Until then ps shows it as "zomb".

static uint32_t to_u32(const char *s, bool *ok) {
    uint32_t v = 0, n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); n++; }
    *ok = (n > 0 && !*s);
    return v;
}

void module_main(int argc, char **argv) {
    myrtos_line_t l;

    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: kill <pid> ...\r\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        bool ok;
        uint32_t pid = to_u32(argv[i], &ok);
        if (ok && myrtos_kill((int32_t)pid) == 0) continue;

        myrtos_line_reset(&l);
        myrtos_line_str(&l, "kill: ");
        myrtos_line_str(&l, argv[i]);
        myrtos_line_str(&l, ok ? ": no such process, or the kernel needs it\r\n"
                               : ": not a number\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }
}
