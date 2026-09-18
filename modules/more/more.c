#include "../../common/ubiqos_stdio.h"

// more -- a file or standard input, a screenful at a time.
//
//     more /var/dmesg
//     cat big.txt | more
//
// The console cannot be scrolled back, so anything longer than the screen is
// gone the moment it is printed. This is the whole reason to have it.
//
// Space shows the next screenful, Return one more line, q gives up.

UBIQOS_LIBC_DEFINE
UBIQOS_MEM_SIZE(8192);

// The keys are read from descriptor 2, not from standard input, and that is
// the point: "cat x | more" has replaced standard input with the pipe, and a
// more that waited there would be waiting for the file to press a key. Two is
// still the terminal, which is where the reader's hands are.
#define KEYS UBIQOS_STDERR

static int screen_rows(void) {
    ubiqos_confont_t f;
    if (ubiqos_console_font_info(-1, &f) < 0 || !f.rows) return 24;  // no screen
    return f.rows;
}

// True to carry on.
static bool pause_here(int *left, int rows) {
    fputs("-- more --", stderr);
    for (;;) {
        char c;
        if (ubiqos_read(KEYS, &c, 1) != 1) return false;    // nothing more to ask
        // Wipe the prompt, so the text that follows starts in a clean line.
        fputs("\r          \r", stderr);
        if (c == 'q' || c == 'Q') return false;
        if (c == '\r' || c == '\n') { *left = 1; return true; }
        *left = rows - 1;
        return true;
    }
}

void module_main(int argc, char **argv)
{
    if (ubiqos_help(argc, argv,
            "usage: more [FILE]\n\nA screenful at a time; standard input when no file is named.\nSpace for the next screen, q to stop.\n")) return;

    FILE *f = stdin;
    bool ours = false;

    if (argc >= 2) {
        struct stat st;
        if (stat(argv[1], &st) < 0) { fputs("more: no such file\n", stderr); return; }
        if (S_ISDIR(st.st_mode)) { fputs("more: that is a directory\n", stderr); return; }
        f = fopen(argv[1], "r");
        if (!f) { fputs("more: cannot open it\n", stderr); return; }
        ours = true;
    }

    int rows = screen_rows();
    int left = rows - 1;                    // one line kept for the prompt
    char line[160];

    while (fgets(line, (int)sizeof line, f)) {
        fputs(line, stdout);
        if (--left > 0) continue;
        if (!pause_here(&left, rows)) break;
    }

    if (ours) fclose(f);
}
