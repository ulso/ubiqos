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

// How tall is the terminal? ASK IT, and fall back to the screen's own size.
//
// The screen is the wrong answer over a network: the board's console is thirty
// rows whatever the window at the other end is, so `more` paused in the middle
// of a forty-row terminal and stopped in the wrong place.
//
// ESC [ 18 t is the question -- "how big is the text area" -- and a terminal
// answers ESC [ 8 ; rows ; cols t. It is asked rather than the cursor-position
// trick because that one has to drive the cursor into the far corner to find
// out, and this console, which drops sequences it does not know, would have
// been left with the cursor there. Dropped is exactly what happens here, so on
// the screen there is no answer and the font's own row count is used, which is
// right.
static bool ask_terminal(int *rows) {
    fputs("\x1b[18t", stderr);
    fflush(stderr);

    const uint32_t deadline = ubiqos_ticks_now() + 150;
    int state = 0;                       // 0 esc, 1 '[', 2 the first number,
    uint32_t num = 0, first = 0;         // 3 the second
    while ((int32_t)(ubiqos_ticks_now() - deadline) < 0) {
        char ch;
        if (ubiqos_readable(KEYS) <= 0) { ubiqos_sleep(2); continue; }
        if (ubiqos_read(KEYS, &ch, 1) <= 0) continue;
        if (state == 0) { if (ch == 0x1b) state = 1; continue; }
        if (state == 1) { state = (ch == '[') ? 2 : 0; num = 0; continue; }
        if (ch >= '0' && ch <= '9') { num = num * 10 + (uint32_t)(ch - '0'); continue; }
        if (ch == ';') {
            if (state == 2) { first = num; state = 3; }
            else if (state == 3 && first == 8) { *rows = (int)num; return num >= 4; }
            num = 0;
            continue;
        }
        state = 0;                       // anything else ends it
        num = 0;
    }
    return false;
}

static int screen_rows(void) {
    int rows = 0;
    if (ask_terminal(&rows) && rows >= 4 && rows <= 200) return rows;
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
        // Ctrl-C stops it. Without this it counted as "any other key" and
        // turned a page, which over a network connection is the only way to
        // stop anything: there is no terminal driver in that path to notice
        // the key and end the command for you.
        if (c == 'q' || c == 'Q' || c == 3) return false;
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
