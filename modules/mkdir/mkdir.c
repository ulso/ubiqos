#include "../../common/myrtos_abi.h"

// mkdir -- makes a directory on the SD card.
//
// The path is absolute and slash-separated, and every component but the last
// must already exist: this makes one directory, not a chain of them. That is
// what mkdir does everywhere without -p, and the recursive form can be added
// when something wants it.
void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: mkdir DIRECTORY\n")) return;

    myrtos_line_t line;

    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: mkdir <path>\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        myrtos_line_reset(&line);
        if (myrtos_mkdir(argv[i]) == 0) {
            myrtos_line_str(&line, "created ");
        } else {
            // Either the parent is missing, the name is taken, or the card is
            // full. The filesystem does not say which, and guessing in the
            // message would be worse than not saying.
            myrtos_line_str(&line, "mkdir: cannot create ");
        }
        myrtos_line_str(&line, argv[i]);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
    }
}
