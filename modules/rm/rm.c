#include "../../common/myrtos_abi.h"

// rm -- removes files from the card. Directories are refused: the reader only
// knows the root, so a directory could be unlinked but never inspected first.

void module_main(int argc, char **argv) {
    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDERR, "usage: rm FILE...\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (myrtos_fs_remove(argv[i]) == 0) continue;
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "rm: ");
        myrtos_line_str(&l, argv[i]);
        myrtos_line_str(&l, ": not removed\n");
        myrtos_line_flush(MYRTOS_STDERR, &l);
    }
}
