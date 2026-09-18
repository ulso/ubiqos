#include "../../common/ubiqos_abi.h"

// mv -- another name for a file, and on one volume that is all it is.
//
// It is not cp followed by rm. On FAT the name lives in a directory entry and
// the contents live in a cluster chain the entry points at, so renaming edits
// two entries and moves no data at all -- a hundred-kilobyte file costs the
// same as an empty one. Copying would move every byte through this machine to
// arrive where it already was.
//
// Across volumes it refuses rather than falling back on a copy. The
// filesystems share no cluster chain, so it would be a different operation
// with different failure modes: a copy that runs out of room halfway has
// written a partial file and not yet deleted the original, and calling that
// "mv" is how somebody loses the file they were moving. Say no and let them
// type cp.
static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: mv SOURCE DEST\n\n"
            "Renames a file, which also moves it between directories on the same\n"
            "volume. It does not copy, so it will not move between volumes.\n"))
        return;

    if (argc != 3) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: mv SOURCE DEST\n");
        return;
    }
    if (is(argv[1], argv[2])) return;             // nothing to do, and not an error

    if (ubiqos_fs_rename(argv[1], argv[2]) == 0) return;

    // One message, and it names the likely causes rather than the operation.
    // "mv: failed" sends the reader to look at the card.
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "mv: cannot rename ");
    ubiqos_line_str(&l, argv[1]);
    ubiqos_line_str(&l, "\n     -- no such file, the destination exists, or the two\n"
                        "        are on different volumes, which mv does not cross\n");
    ubiqos_line_flush(UBIQOS_STDERR, &l);
}
