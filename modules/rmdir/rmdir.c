#include "../../common/myrtos_abi.h"

// rmdir -- removes a directory, provided it is empty.
//
// Separate from rm, as it is everywhere: deleting a whole tree because a name
// was mistyped is not something a utility should be able to do by accident.
void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: rmdir DIRECTORY\n\nIt must be empty.\n")) return;

    myrtos_line_t line;

    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: rmdir <path>\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (myrtos_rmdir(argv[i]) == 0) continue;
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "rmdir: cannot remove ");
        myrtos_line_str(&line, argv[i]);
        myrtos_line_str(&line, " (not there, not a directory, or not empty)\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
    }
}
