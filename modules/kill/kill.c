#include "../../common/ubiqos_abi.h"

// kill -- end a process by number, or every process running a module by name.
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

static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// By name, every one of them: `kill httpd` is what somebody who started it
// with & in /sd/startup can type without first asking ps for a number. The
// table is read once, before anything is ended, so a process that is given
// its half second to stop is not found a second time and asked again.
static int32_t kill_by_name(const char *name, bool now) {
    int32_t pids[UBIQOS_PS_SLOTS];
    uint32_t n = 0;
    for (uint32_t slot = 0; slot < UBIQOS_PS_SLOTS; slot++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(slot, &p) < 0) continue;
        if (same(p.name, name)) pids[n++] = (int32_t)p.pid;
    }
    int32_t ended = 0;
    for (uint32_t i = 0; i < n; i++)
        if ((now ? ubiqos_kill_now(pids[i]) : ubiqos_kill(pids[i])) == 0) ended++;
    return n ? ended : -1;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: kill [-9] <pid|name> ...\n\nAsks the process to end -- by number, or every one running the module\nof that name, as ps shows it: 'kill httpd'. One that catches the interrupt is told and\nhas half a second to stop itself.\n  -9   end it at once, asking nothing\n")) return;

    ubiqos_line_t l;

    int first = 1;
    bool now = false;

    // -9 ends without asking. Without it a process that called
    // ubiqos_catch_intr is told and given half a second to end itself, which is
    // how a background scanner gets to tell its dongle to stop -- Ctrl-C cannot
    // reach it, because a background process is nobody's foreground.
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '9' && !argv[1][2]) {
        now = true;
        first = 2;
    }

    if (argc < first + 1) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: kill [-9] <pid|name> ...\r\n");
        return;
    }

    for (int i = first; i < argc; i++) {
        bool ok;
        uint32_t pid = to_u32(argv[i], &ok);
        if (!ok) {
            int32_t r = kill_by_name(argv[i], now);
            if (r > 0) continue;
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "kill: ");
            ubiqos_line_str(&l, argv[i]);
            ubiqos_line_str(&l, r < 0 ? ": nothing running by that name\r\n"
                                      : ": the kernel needs it\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            continue;
        }
        int32_t r = now ? ubiqos_kill_now((int32_t)pid) : ubiqos_kill((int32_t)pid);
        if (r == 0) continue;

        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "kill: ");
        ubiqos_line_str(&l, argv[i]);
        ubiqos_line_str(&l, ": no such process, or the kernel needs it\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}
