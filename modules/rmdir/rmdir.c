#include "../../common/ubiqos_abi.h"

// rmdir -- removes a directory, provided it is empty.
//
// Separate from rm, as it is everywhere: deleting a whole tree because a name
// was mistyped is not something a utility should be able to do by accident.
void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: rmdir DIRECTORY\n\nIt must be empty.\n")) return;

    ubiqos_line_t line;

    if (argc < 2) {
        ubiqos_write_str(UBIQOS_STDOUT, "usage: rmdir <path>\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (ubiqos_rmdir(argv[i]) == 0) continue;
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "rmdir: cannot remove ");
        ubiqos_line_str(&line, argv[i]);
        ubiqos_line_str(&line, " (not there, not a directory, or not empty)\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
    }
}
