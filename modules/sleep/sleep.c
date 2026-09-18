#include "../../common/ubiqos_abi.h"

// sleep -- waits for a length of time and says nothing, as sleep does
// everywhere. The unit is milliseconds: the tick is one, and a system with no
// clock has little use for a coarser one.

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
    if (ubiqos_help(argc, argv,
            "usage: sleep MILLISECONDS\n")) return;

    uint32_t ms;
    if (argc != 2 || !parse_u32(argv[1], &ms)) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: sleep MILLISECONDS\n");
        return;
    }
    ubiqos_sleep(ms);
}
