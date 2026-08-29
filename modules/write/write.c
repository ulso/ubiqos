#include "../../common/myrtos_abi.h"

// write -- puts its arguments into a file, separated by spaces.
//
//     write notes.txt hello myrtos
//
// The shell has no redirection, so without this there is no way to create a
// file from the keyboard at all. It is the smallest thing that closes that gap,
// and where an editor would start.

void module_main(int argc, char **argv) {
    if (argc < 3) {
        myrtos_write_str(MYRTOS_STDERR, "usage: write FILE TEXT...\n");
        return;
    }

    // Replacing, not appending: writing does not truncate, so a shorter text
    // over a longer file would otherwise leave the old tail in place.
    myrtos_fs_remove(argv[1]);

    myrtos_line_t line;
    myrtos_line_reset(&line);
    for (int i = 2; i < argc; i++) {
        if (i > 2) myrtos_line_str(&line, " ");
        myrtos_line_str(&line, argv[i]);
    }
    myrtos_line_str(&line, "\n");

    int32_t n = myrtos_fs_write(argv[1], 0, line.buf, line.len);
    if (n == (int32_t)line.len) return;

    myrtos_line_t err;
    myrtos_line_reset(&err);
    myrtos_line_str(&err, "write: ");
    myrtos_line_str(&err, argv[1]);
    myrtos_line_str(&err, ": failed\n");
    myrtos_line_flush(MYRTOS_STDERR, &err);
}
