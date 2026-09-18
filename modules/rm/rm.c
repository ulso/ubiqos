#include "../../common/ubiqos_abi.h"

// rm -- removes files from the card. Directories are refused: the reader only
// knows the root, so a directory could be unlinked but never inspected first.

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: rm FILE...\n")) return;

    if (argc < 2) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: rm FILE...\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (ubiqos_fs_remove(argv[i]) == 0) continue;
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "rm: ");
        ubiqos_line_str(&l, argv[i]);
        ubiqos_line_str(&l, ": not removed\n");
        ubiqos_line_flush(UBIQOS_STDERR, &l);
    }
}
