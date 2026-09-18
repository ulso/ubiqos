#include "../../common/ubiqos_abi.h"

// mkdir -- makes a directory on the SD card.
//
// The path is absolute and slash-separated, and every component but the last
// must already exist: this makes one directory, not a chain of them. That is
// what mkdir does everywhere without -p, and the recursive form can be added
// when something wants it.
void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: mkdir DIRECTORY\n")) return;

    ubiqos_line_t line;

    if (argc < 2) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: mkdir <path>\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        ubiqos_line_reset(&line);
        if (ubiqos_mkdir(argv[i]) == 0) {
            ubiqos_line_str(&line, "created ");
        } else {
            // Either the parent is missing, the name is taken, or the card is
            // full. The filesystem does not say which, and guessing in the
            // message would be worse than not saying.
            ubiqos_line_str(&line, "mkdir: cannot create ");
        }
        ubiqos_line_str(&line, argv[i]);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
    }
}
