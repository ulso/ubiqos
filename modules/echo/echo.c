#include "../../common/myrtos_abi.h"

// echo -- prints its arguments separated by spaces. The first utility to use
// argc and argv rather than the raw command line.
void module_main(int argc, char **argv) {
    myrtos_line_t line;
    myrtos_line_reset(&line);

    // argv[0] is the module name and is not printed, as in echo everywhere.
    for (int i = 1; i < argc; i++) {
        if (i > 1) myrtos_line_str(&line, " ");
        myrtos_line_str(&line, argv[i]);
    }
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
