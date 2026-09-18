#include "../../common/ubiqos_abi.h"

// ls -- lists a directory on the SD card, the root when given no path.
//
// The kernel hands back raw 8.3 names, eleven characters with no dot and padded
// with spaces. Presenting them as "sh.mod" is this utility's business: the
// filesystem stores what FAT stores, and the shape a person reads is a matter
// for whoever is doing the reading.

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: ls [DIRECTORY]\n\nLists a directory, or the current one when given no path.\n")) return;

    const char *path = (argc > 1) ? argv[1] : "";
    ubiqos_line_t line;
    char raw[UBIQOS_DIRNAME_MAX], name[UBIQOS_DIRNAME_MAX + 2];
    uint32_t size;
    uint32_t files = 0, bytes = 0;

    for (uint32_t i = 0; ; i++) {
        int32_t attr = ubiqos_fs_dir_at(path, i, raw, &size);
        if (attr < 0) {
            if (i == 0) {
                // Empty is not missing, and until stat existed there was no way
                // to tell them apart: an empty directory has no first entry to
                // list, and neither has one that is not there. An empty /tmp
                // reported itself as missing for exactly that reason.
                uint32_t size = 0;
                int32_t st = ubiqos_fs_stat(path[0] ? path : "/", &size);
                if (st >= 0 && (st & UBIQOS_ATTR_DIRECTORY)) break;

                ubiqos_line_reset(&line);
                // Nothing is mounted at startup any more, so the root failing
                // is now the ordinary case rather than a typo. A mounted card
                // always has a root -- even an empty directory answers with
                // "." -- so the root is the one path whose absence says which
                // of the two it was.
                if (!path[0] || (path[0] == '/' && !path[1])) {
                    ubiqos_line_str(&line, "ls: nothing is mounted -- try: mount\n");
                } else {
                    ubiqos_line_str(&line, "ls: no such directory: ");
                    ubiqos_line_str(&line, path);
                    ubiqos_line_str(&line, "\n");
                }
                ubiqos_line_flush(UBIQOS_STDOUT, &line);
                return;
            }
            break;
        }

        // The rule is in common/ubiqos_abi.h; ls, readdir and the wasm host
        // all have to expand a FAT name the same way.
        ubiqos_pretty_name(raw, name);
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, name);

        // Pad to a column so the sizes line up. Sixteen is wider than any 8.3
        // name can be, so a name never pushes its own size out of line.
        uint32_t n = 0;
        while (name[n]) n++;
        // A long name can be wider than the column, and then one space is what
        // keeps the size from running into it.
        if (n >= 16) ubiqos_line_str(&line, " ");
        else while (n++ < 16) ubiqos_line_str(&line, " ");

        if (attr & UBIQOS_ATTR_DIRECTORY) {
            ubiqos_line_str(&line, "<dir>");
        } else {
            ubiqos_line_u32(&line, size);
            files++;
            bytes += size;
        }
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
    }

    ubiqos_line_reset(&line);
    ubiqos_line_u32(&line, files);
    ubiqos_line_str(&line, files == 1 ? " file, " : " files, ");
    ubiqos_line_u32(&line, bytes);
    ubiqos_line_str(&line, " bytes\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);
}
