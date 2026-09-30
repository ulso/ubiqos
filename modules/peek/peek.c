#include "../../common/ubiqos_abi.h"

// peek ADDRESS [COUNT] -- words of memory, or a peripheral's registers, in hex.
//
// A program here runs privileged, so it can read what a debugger would. It is
// what bringing a board up needs when the debugger cannot reach it: on iLabs'
// Challenger+ the probe would not connect, and the question was what the UART
// and the pins were actually set to rather than what the driver meant them to
// be. Reading an address that is not there faults -- which ends this program
// and nothing else.
static bool hex(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        const char c = *s;
        const uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                         : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16u;
        if (d > 15u) return false;
        v = v * 16u + d;
    }
    *out = v;
    return true;
}

static void put_hex(ubiqos_line_t *l, uint32_t v) {
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = (v >> (i * 4)) & 15u;
        const char c[2] = { (char)(d < 10u ? '0' + d : 'a' + d - 10u), 0 };
        ubiqos_line_str(l, c);
    }
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: peek ADDRESS [COUNT]\n\nCOUNT words from ADDRESS, in hex; both in hex.\n")) return;
    uint32_t addr = 0, count = 1;
    if (argc < 2 || !hex(argv[1], &addr) || (argc > 2 && !hex(argv[2], &count))) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: peek ADDRESS [COUNT]\n");
        return;
    }
    addr &= ~3u;
    for (uint32_t i = 0; i < count; i += 4) {
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        put_hex(&l, addr + i * 4u);
        ubiqos_line_str(&l, ":");
        for (uint32_t j = i; j < count && j < i + 4u; j++) {
            ubiqos_line_str(&l, " ");
            put_hex(&l, *(volatile const uint32_t *)(uintptr_t)(addr + j * 4u));
        }
        ubiqos_line_str(&l, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}
