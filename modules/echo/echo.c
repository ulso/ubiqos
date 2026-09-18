#include "../../common/ubiqos_abi.h"

// echo -- prints its arguments separated by spaces. The first utility to use
// argc and argv rather than the raw command line.
void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: echo [TEXT...]\n\nPrints its arguments separated by spaces.\n")) return;

    ubiqos_line_t line;
    ubiqos_line_reset(&line);

    // argv[0] is the module name and is not printed, as in echo everywhere.
    for (int i = 1; i < argc; i++) {
        if (i > 1) ubiqos_line_str(&line, " ");
        ubiqos_line_str(&line, argv[i]);
    }
    ubiqos_line_str(&line, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);
}
