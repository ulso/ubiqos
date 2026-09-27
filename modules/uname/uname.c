#include "../../common/ubiqos_stdio.h"

UBIQOS_LIBC_DEFINE

// uname -- what this system is.
//
//     ubiqos:/> uname -a
//     UbiqOS fruit-jam 0.1.16 Sep 27 2026 11:52:03 armv8m cortex-m33 Adafruit Fruit Jam
//
// The fields are the POSIX ones and GNU's two extras, in the usual order:
// -s the system, -n the machine's name on the network, -r the release, -v the
// build, -m the architecture, -p the processor and -i the board. With no option
// it says the system, as uname always has; -a says everything.
//
// The kernel is asked for what only it knows -- the name, the release, the
// board and when it was built -- and the architecture this program knows for
// itself, because it was built for one: a module compiled for the Hazard3 cores
// is running on them.

#ifdef __riscv
#define MACHINE   "riscv32"
#define PROCESSOR "hazard3"
#else
#define MACHINE   "armv8m"
#define PROCESSOR "cortex-m33"
#endif

static void config(uint32_t what, char *out, uint32_t cap) {
    if (ubiqos_syscall(SYS_CONFIG, what, (uint32_t)(uintptr_t)out, cap) <= 0 || !out[0]) {
        out[0] = '?';
        out[1] = 0;
    }
}

enum { F_S = 1, F_N = 2, F_R = 4, F_V = 8, F_M = 16, F_P = 32, F_I = 64, F_ALL = 127 };

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: uname [-asnrvmpi]\n\n"
            "  -s  the system, UbiqOS (the default)\n"
            "  -n  this machine's name on the network\n"
            "  -r  the release, as the boot banner gives it\n"
            "  -v  when the kernel was built\n"
            "  -m  the architecture: armv8m or riscv32\n"
            "  -p  the processor: cortex-m33 or hazard3\n"
            "  -i  the board\n"
            "  -a  all of them, in that order\n"))
        return;

    uint32_t want = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) { printf("uname: usage: uname [-asnrvmpi]\n"); return; }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'a': want |= F_ALL; break;
            case 's': want |= F_S; break;
            case 'n': want |= F_N; break;
            case 'r': want |= F_R; break;
            case 'v': want |= F_V; break;
            case 'm': want |= F_M; break;
            case 'p': want |= F_P; break;
            case 'i': want |= F_I; break;
            default:
                printf("uname: no option -%c; try uname -h\n", *c);
                return;
            }
        }
    }
    if (!want) want = F_S;

    char name[40], release[40], built[32], board[48];
    config(UBIQOS_CFG_HOSTNAME, name, sizeof name);
    config(UBIQOS_CFG_VERSION, release, sizeof release);
    config(UBIQOS_CFG_BUILT, built, sizeof built);
    config(UBIQOS_CFG_BOARD, board, sizeof board);

    const char *field[7] = { "UbiqOS", name, release, built, MACHINE, PROCESSOR, board };
    bool first = true;
    for (int i = 0; i < 7; i++) {
        if (!(want & (1u << i))) continue;
        printf(first ? "%s" : " %s", field[i]);
        first = false;
    }
    printf("\n");
}
