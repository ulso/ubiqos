#include "../../common/myrtos_abi.h"

// echo -- skriver ut sina argument, åtskilda av blanksteg. Första verktyget
// som använder argc och argv i stället för den råa kommandoraden.
void module_main(int argc, char **argv) {
    myrtos_line_t line;
    myrtos_line_reset(&line);

    // argv[0] är modulnamnet och skrivs inte ut, precis som i echo överallt.
    for (int i = 1; i < argc; i++) {
        if (i > 1) myrtos_line_str(&line, " ");
        myrtos_line_str(&line, argv[i]);
    }
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
