#include "../../common/ubiqos_stdio.h"

// write -- puts its arguments into a file, separated by spaces.
//
//     write notes.txt hello UbiqOS
//     write -a notes.txt and some more
//
// The shell has no redirection, so without this there is no way to create a
// file from the keyboard at all. It is the smallest thing that closes that gap,
// and where an editor would start.
//
// Written against ubiqos_stdio.h, and the writing counterpart to head: fopen,
// fputs, fclose, and "a" for appending -- which only became possible when stat
// arrived and open could ask how long the file already was.

UBIQOS_LIBC_DEFINE
UBIQOS_MEM_SIZE(8192);

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: write [-a] FILE TEXT...\n\nPuts its arguments into a file, separated by spaces.\n  -a   append instead of replacing\n")) return;

    int arg = 1;
    const char *mode = "w";

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'a' && !argv[1][2]) {
        mode = "a";
        arg = 2;
    }

    if (argc < arg + 2) {
        fputs("usage: write [-a] FILE TEXT...\n", stderr);
        return;
    }

    // "w" truncates, which matters: writing does not shorten a file, so a
    // shorter text over a longer one would otherwise leave the old tail behind.
    FILE *f = fopen(argv[arg], mode);
    if (!f) {
        fputs("write: cannot open it\n", stderr);
        return;
    }

    for (int i = arg + 1; i < argc; i++) {
        if (i > arg + 1) fputc(' ', f);
        fputs(argv[i], f);
    }
    fputc('\n', f);

    if (fclose(f) == EOF) fputs("write: failed\n", stderr);
}
