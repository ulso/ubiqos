#include "../../common/myrtos_abi.h"

// ls -- lists a directory on the SD card, the root when given no path.
//
// The kernel hands back raw 8.3 names, eleven characters with no dot and padded
// with spaces. Presenting them as "sh.mod" is this utility's business: the
// filesystem stores what FAT stores, and the shape a person reads is a matter
// for whoever is doing the reading.

// "SH      MOD" -> "sh.mod". A directory has no extension worth showing.
static void pretty(const char *raw, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = raw[i];
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = raw[i];
            out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[n] = 0;
}

void module_main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "";
    myrtos_line_t line;
    char raw[12], name[14];
    uint32_t size;
    uint32_t files = 0, bytes = 0;

    for (uint32_t i = 0; ; i++) {
        int32_t attr = myrtos_fs_dir_at(path, i, raw, &size);
        if (attr < 0) {
            if (i == 0) {
                // Empty is not missing, and until stat existed there was no way
                // to tell them apart: an empty directory has no first entry to
                // list, and neither has one that is not there. An empty /tmp
                // reported itself as missing for exactly that reason.
                uint32_t size = 0;
                int32_t st = myrtos_fs_stat(path[0] ? path : "/", &size);
                if (st >= 0 && (st & MYRTOS_ATTR_DIRECTORY)) break;

                myrtos_line_reset(&line);
                // Nothing is mounted at startup any more, so the root failing
                // is now the ordinary case rather than a typo. A mounted card
                // always has a root -- even an empty directory answers with
                // "." -- so the root is the one path whose absence says which
                // of the two it was.
                if (!path[0] || (path[0] == '/' && !path[1])) {
                    myrtos_line_str(&line, "ls: nothing is mounted -- try: mount\n");
                } else {
                    myrtos_line_str(&line, "ls: no such directory: ");
                    myrtos_line_str(&line, path);
                    myrtos_line_str(&line, "\n");
                }
                myrtos_line_flush(MYRTOS_STDOUT, &line);
                return;
            }
            break;
        }

        pretty(raw, name);
        myrtos_line_reset(&line);
        myrtos_line_str(&line, name);

        // Pad to a column so the sizes line up. Sixteen is wider than any 8.3
        // name can be, so a name never pushes its own size out of line.
        uint32_t n = 0;
        while (name[n]) n++;
        while (n++ < 16) myrtos_line_str(&line, " ");

        if (attr & MYRTOS_ATTR_DIRECTORY) {
            myrtos_line_str(&line, "<dir>");
        } else {
            myrtos_line_u32(&line, size);
            files++;
            bytes += size;
        }
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
    }

    myrtos_line_reset(&line);
    myrtos_line_u32(&line, files);
    myrtos_line_str(&line, files == 1 ? " file, " : " files, ");
    myrtos_line_u32(&line, bytes);
    myrtos_line_str(&line, " bytes\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
