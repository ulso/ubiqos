#include "../../common/myrtos_posix.h"

// The one definition of errno the program owes, exactly as a C library would
// have provided it. Thread-local because it is per-process writable state, and
// a shareable module may not have that as a static.
__thread int errno;

// cat -- writes files to standard output.
//
// The file is read a piece at a time rather than whole. A process gets 4 kB for
// data and stack together, so holding a file in memory would put an arbitrary
// ceiling on what cat can show; reading in slices puts none.
//
// Written against myrtos_posix.h rather than the system calls, so this file is
// also the answer to "can ordinary C be built here": open, read, close and
// STDOUT_FILENO, with nothing myrtos-shaped in the loop at all.

// 256 was four hundred read calls for a hundred-kilobyte file, and while the
// filesystem no longer re-walks the FAT chain for each one, every call is still
// a message to the file server and a round trip through it. A kilobyte is two
// sectors, and stays well inside the four kilobytes a process has for data and
// stack together -- which is the ceiling this must not go near, since the
// buffer is a local.
#define CHUNK 1024

// Which failure it was, now that open can tell them apart. "no such file" for a
// directory was true of the open and false of the world, and it is the kind of
// message that sends someone looking for a typo in a name that is spelled
// perfectly.
static void complain(const char *name) {
    const char *why = errno == EISDIR ? ": is a directory\n"
                    : errno == EACCES ? ": this one is not readable\n"
                    : errno == EMFILE ? ": too many open files\n"
                    :                   ": no such file\n";
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "cat: ");
    myrtos_line_str(&l, name);
    myrtos_line_str(&l, why);
    myrtos_line_flush(MYRTOS_STDERR, &l);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: cat FILE...\n\nWrites each file to standard output.\n")) return;

    // No arguments means standard input, as cat has always done -- which is
    // also the only way to see that "< file" reached the child, since every
    // other utility here takes its file by name.
    if (argc < 2) {
        uint8_t buf[CHUNK];
        for (;;) {
            int32_t n = read(STDIN_FILENO, buf, CHUNK);
            if (n <= 0) break;
            write(STDOUT_FILENO, buf, (uint32_t)n);
        }
        return;
    }

    uint8_t buf[CHUNK];
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { complain(argv[i]); continue; }
        for (;;) {
            int32_t n = read(fd, buf, CHUNK);
            // Nought is the end of the file and a negative is a file that was
            // never there. Opening does not check -- it cannot, while there are
            // no flags to say whether a write should create -- so this is where
            // a missing file is discovered, and collapsing the two into one
            // test made cat silent about it.
            if (n < 0) { complain(argv[i]); break; }
            if (n == 0) break;
            write(STDOUT_FILENO, buf, (uint32_t)n);
        }
        close(fd);
    }
}
