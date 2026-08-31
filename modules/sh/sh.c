#include "../../common/myrtos_abi.h"

// The myrtos shell. OS-9's shell did essentially this: read a name, look the
// module up, start it. All the complexity is in the kernel; the shell is a loop.
//
// The loop grew an editor. It is worth saying why it is here rather than in the
// console: this shell runs twice, once on the screen and once on the serial
// port, and the thing at the other end of the serial port is somebody else's
// terminal emulator. Line editing therefore cannot live in the terminal -- one
// of the two terminals is not ours. It lives here, and speaks to both ends in
// the only language they have in common, which is ANSI.

#define LINE_MAX    128
#define HIST_LINES   16
#define PROMPT_MAX   80

// Everything the editor needs, in one place. A module may not have writable
// statics -- one copy would be shared by every process running it, and there
// are two shells -- so this is a local of module_main, passed by pointer.
typedef struct {
    int32_t  out;
    char     prompt[PROMPT_MAX];
    uint32_t prompt_len;
    uint32_t width;              // columns, so a redraw knows where the edge is

    char     line[LINE_MAX];
    uint32_t len, pos;

    // The history is a ring of fixed slots from the kernel's heap: two
    // kilobytes is more than a module's whole memory block, and it is no use on
    // the stack of a process that must also run commands.
    char    *hist;               // HIST_LINES * LINE_MAX, or 0 if there is none
    uint32_t hist_count, hist_next;
    uint32_t browse;             // 0 while editing, else how far back we are
    char     stash[LINE_MAX];    // the line being edited when browsing started
} editor_t;

static char *hist_slot(editor_t *e, uint32_t back) {   // back = 1 is the newest
    uint32_t i = (e->hist_next + HIST_LINES - back) % HIST_LINES;
    return e->hist + i * LINE_MAX;
}

static uint32_t copy_str(char *dst, const char *src, uint32_t max) {
    uint32_t n = 0;
    while (src[n] && n < max - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    return n;
}

// The prompt says where we are, because with directories there is now somewhere
// to be. The root shows as a bare "/" rather than nothing at all. It is built
// rather than printed, because the editor needs to know how wide it is.
static void build_prompt(editor_t *e) {
    char cwd[64];
    myrtos_getcwd(cwd, sizeof(cwd));
    uint32_t n = copy_str(e->prompt, "myrtos:", PROMPT_MAX);
    n += copy_str(e->prompt + n, cwd[0] ? cwd : "/", PROMPT_MAX - n);
    n += copy_str(e->prompt + n, "> ", PROMPT_MAX - n);
    e->prompt_len = n;
}

// Everything from column one again: the prompt, the line, then erase to the end
// of it in case what was there before was longer, then put the cursor where it
// belongs. Absolute column addressing rather than a count of moves, so a
// miscount cannot accumulate.
static void redraw(editor_t *e) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "\r");
    myrtos_line_str(&l, e->prompt);
    myrtos_line_flush(e->out, &l);

    if (e->len) myrtos_write(e->out, e->line, e->len);

    myrtos_line_reset(&l);
    myrtos_line_str(&l, "\x1b[K\r\x1b[");
    myrtos_line_u32(&l, e->prompt_len + e->pos + 1);
    myrtos_line_str(&l, "G");
    myrtos_line_flush(e->out, &l);
}

// How much of a command fits. A line that runs past the right edge wraps, and
// then the carriage return in redraw comes back to the start of the wrapped
// part rather than the start of the line -- so rather than draw it wrong, the
// editor stops taking characters. A long path therefore needs a cd first, which
// is what one would do anyway.
static uint32_t room(editor_t *e) {
    uint32_t r = (e->width > e->prompt_len + 1) ? e->width - e->prompt_len - 1 : 1;
    return r < LINE_MAX - 1 ? r : LINE_MAX - 1;
}

static void set_line(editor_t *e, const char *src) {
    e->len = copy_str(e->line, src, LINE_MAX);
    if (e->len > room(e)) { e->len = room(e); e->line[e->len] = 0; }
    e->pos = e->len;
}

static void insert(editor_t *e, char c) {
    if (e->len >= room(e)) return;
    for (uint32_t i = e->len; i > e->pos; i--) e->line[i] = e->line[i - 1];
    e->line[e->pos++] = c;
    e->len++;
    e->line[e->len] = 0;
}

static void delete_at(editor_t *e, uint32_t at) {
    if (at >= e->len) return;
    for (uint32_t i = at; i < e->len; i++) e->line[i] = e->line[i + 1];
    e->len--;
}

static void remember(editor_t *e) {
    if (!e->hist || !e->len) return;
    // Not the same as the last one. Repeating a command should not push the one
    // before it out of reach.
    if (e->hist_count) {
        const char *prev = hist_slot(e, 1);
        uint32_t i = 0;
        while (i < e->len && prev[i] == e->line[i]) i++;
        if (i == e->len && !prev[i]) return;
    }
    copy_str(e->hist + e->hist_next * LINE_MAX, e->line, LINE_MAX);
    e->hist_next = (e->hist_next + 1) % HIST_LINES;
    if (e->hist_count < HIST_LINES) e->hist_count++;
}

static void browse_back(editor_t *e) {
    if (!e->hist || e->browse >= e->hist_count) return;
    if (e->browse == 0) copy_str(e->stash, e->line, LINE_MAX);
    e->browse++;
    set_line(e, hist_slot(e, e->browse));
}

static void browse_forward(editor_t *e) {
    if (!e->hist || e->browse == 0) return;
    e->browse--;
    set_line(e, e->browse ? hist_slot(e, e->browse) : e->stash);
}

// --- THE WIDTH ------------------------------------------------------------
// Move a long way right, which every terminal clamps at its own edge, then ask
// where the cursor is. The answer is the width. Both ends of this shell answer
// it: a real emulator because it has since the VT100, and our console because
// it was taught to -- see report_position in kernel/console.c.
//
// A terminal that says nothing is not a fault. Eighty is what a serial line has
// been since before any of this, and it is the safe guess.
#define WIDTH_DEFAULT  80
#define WIDTH_WAIT_MS 150

static uint32_t ask_width(int32_t out, int32_t in) {
    myrtos_write_str(out, "\x1b[999C\x1b[6n");

    uint32_t deadline = myrtos_ticks_now() + WIDTH_WAIT_MS;
    uint32_t col = 0, seen = 0;
    int state = 0;                       // 0 esc, 1 '[', 2 row, 3 col
    while ((int32_t)(myrtos_ticks_now() - deadline) < 0) {
        uint8_t ch;
        // Ask before reading. myrtos_read blocks when there is nothing, so
        // reading blindly here waited for the answer that was never coming --
        // until the first keystroke arrived, which it then ate. That is where
        // "echo one" came back as "cho one".
        if (myrtos_readable(in) <= 0) { myrtos_sleep(2); continue; }
        if (myrtos_read(in, &ch, 1) <= 0) continue;
        if (state == 0) { if (ch == 0x1b) state = 1; }
        else if (state == 1) { state = (ch == '[') ? 2 : 0; }
        else if (state == 2) { if (ch == ';') state = 3; else if (ch < '0' || ch > '9') state = 0; }
        else {
            if (ch >= '0' && ch <= '9') { col = col * 10 + (uint32_t)(ch - '0'); seen = 1; }
            else { if (ch == 'R' && seen) break; state = 0; col = 0; seen = 0; }
        }
    }
    myrtos_write_str(out, "\r");
    return (seen && col >= 20 && col <= 400) ? col : WIDTH_DEFAULT;
}

// --- THE LOOP -------------------------------------------------------------

static bool line_is(const char *line, const char *word) {
    while (*word) { if (*line != *word) return false; line++; word++; }
    return *line == 0;
}

static bool line_starts(const char *line, const char *word) {
    while (*word) { if (*line != *word) return false; line++; word++; }
    return true;
}

// cd is built in and has to be. A module changing its own current directory
// changes nothing for the shell that started it -- the child gets a copy at
// exec and takes it to the grave.
static void change_dir(int32_t c, const char *line) {
    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    if (!*arg) arg = "/";

    if (myrtos_chdir(arg) == 0) return;

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "cd: no such directory: ");
    myrtos_line_str(&l, arg);
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(c, &l);
}

static void help(int32_t c) {
    myrtos_write_str(c,
        // The name column is nine wide because bootsel is seven characters and
        // needs two after it. Everything lines up on the same column.
        "\r\nType a module name to run it. Built in:\r\n"
        "  help     this text\r\n"
        "  lsmod    list modules\r\n"
        "  ps       list processes\r\n"
        "  keys     read the USB keyboard\r\n"
        "  free     memory and processes\r\n"
        "  echo     print its arguments\r\n"
        "  ls       list a directory\r\n"
        "  cd       change directory (built in)\r\n"
        "  pwd      where you are\r\n"
        "  mkdir    make a directory\r\n"
        "  rmdir    remove an empty directory\r\n"
        "  mount    take the SD card again\r\n"
        "  wifi     firmware, or 'wifi scan' for networks\r\n"
        "  font     screen font, or 'font 6x12' to change it\r\n"
        "  color    text and background, or 'color' to see them\r\n"
        "  cat      show a file\r\n"
        "  cp       copy a file\r\n"
        "  rm       delete a file\r\n"
        "  write    write text to a file\r\n"
        "  sleep    wait, in milliseconds\r\n"
        "  nice     run a command at a priority\r\n"
        "  bootsel  reboot into the bootloader\r\n"
        "\r\nA trailing & runs a command without waiting for it.\r\n"
        "Arrows move along the line and up and down the history; ctrl-A and\r\n"
        "ctrl-E jump to its ends; ctrl-L clears the screen when it is empty.\r\n");
}

// Split the line at the first space: everything before is the module name,
// everything after is the command line the process is given.
static int32_t exec_line(char *line) {
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) { *args = 0; args++; while (*args == ' ') args++; }

    // A trailing & means do not wait. Without it there is no way to have two
    // processes running at once from the keyboard, and no way to see that the
    // sleep list orders more than one sleeper.
    bool background = false;
    char *end = args;
    while (*end) end++;
    while (end > args && end[-1] == ' ') end--;
    if (end > args && end[-1] == '&') {
        background = true;
        end[-1] = 0;
        while (end > args && end[-1] == ' ') { end--; *end = 0; }
    }

    int32_t pid = myrtos_exec(line, args);
    // Wait for it before prompting again. Without this the prompt raced the
    // command's own output, and two commands in a row interleaved their lines.
    if (pid >= 0 && !background) myrtos_wait(pid);
    return pid;
}

void module_main(void) {
    // The kernel has already given us 0, 1 and 2. The shell opens nothing.
    editor_t ed;
    editor_t *e = &ed;

    e->out = MYRTOS_STDOUT;
    e->len = e->pos = 0;
    e->line[0] = 0;
    e->stash[0] = 0;
    e->hist_count = e->hist_next = e->browse = 0;
    e->hist = (char*)myrtos_alloc(HIST_LINES * LINE_MAX);

    myrtos_write_str(e->out, "\r\nmyrtos shell ready. Type 'help'.\r\n");
    e->width = ask_width(e->out, MYRTOS_STDIN);
    build_prompt(e);
    redraw(e);

    int state = 0;                       // 0 text, 1 after ESC, 2 in CSI
    uint32_t csi_num = 0;

    for (;;) {
        uint8_t ch;
        if (myrtos_read(MYRTOS_STDIN, &ch, 1) <= 0) continue;   // nothing right now

        // The same sequences the console draws are the ones the keyboard sends,
        // so one state machine serves the screen and the serial port. See
        // nav_sequence in kernel/usbhost.c.
        if (state == 1) {
            state = (ch == '[') ? 2 : 0;
            csi_num = 0;
            continue;
        }
        if (state == 2) {
            if (ch >= '0' && ch <= '9') { csi_num = csi_num * 10 + (uint32_t)(ch - '0'); continue; }
            state = 0;
            switch (ch) {
            case 'A': browse_back(e);    redraw(e); break;
            case 'B': browse_forward(e); redraw(e); break;
            case 'C': if (e->pos < e->len) { e->pos++; redraw(e); } break;
            case 'D': if (e->pos)         { e->pos--; redraw(e); } break;
            case 'H': e->pos = 0;      redraw(e); break;
            case 'F': e->pos = e->len; redraw(e); break;
            case '~':
                if (csi_num == 3) { delete_at(e, e->pos); redraw(e); }   // Delete
                else if (csi_num == 1 || csi_num == 7) { e->pos = 0;      redraw(e); }
                else if (csi_num == 4 || csi_num == 8) { e->pos = e->len; redraw(e); }
                break;
            default: break;
            }
            continue;
        }

        if (ch == 0x1b) { state = 1; continue; }

        if (ch == '\r' || ch == '\n') {
            e->line[e->len] = 0;
            myrtos_write_str(e->out, "\r\n");
            bool ran = e->len != 0;
            if (ran) {
                remember(e);
                if (line_is(e->line, "help")) {
                    help(e->out);
                } else if (line_is(e->line, "cd") || line_starts(e->line, "cd ")) {
                    change_dir(e->out, e->line);
                } else {
                    int32_t r = exec_line(e->line);
                    if (r < 0) {
                        myrtos_line_t l;
                        myrtos_line_reset(&l);
                        // -2 means the module is there but is not re-entrant and
                        // is already running. Saying "no such module" for that
                        // sends the reader looking for the wrong problem.
                        myrtos_line_str(&l, r == -2 ? "already running: "
                                                    : "no such module: ");
                        myrtos_line_str(&l, e->line);  // exec_line NUL-terminated the name
                        myrtos_line_str(&l, "\r\n");
                        myrtos_line_flush(e->out, &l);
                    }
                }
            }
            e->len = e->pos = e->browse = 0;
            e->line[0] = 0;
            build_prompt(e);             // cd may have moved us
            // A blank line between a command's output and the next prompt, as
            // there has always been. Nothing ran, nothing to separate.
            if (ran) myrtos_write_str(e->out, "\r\n");
            redraw(e);
        } else if (ch == 1) {            // Ctrl-A
            e->pos = 0; redraw(e);
        } else if (ch == 5) {            // Ctrl-E
            e->pos = e->len; redraw(e);
        } else if (ch == 12) {           // Ctrl-L, but only on an empty line
            if (!e->len) { myrtos_write_str(e->out, "\x1b[2J\x1b[H"); redraw(e); }
        } else if (ch == 8 || ch == 127) {
            if (e->pos) { e->pos--; delete_at(e, e->pos); redraw(e); }
        } else if (ch >= ' ' && ch < 0x7f) {
            insert(e, (char)ch);
            redraw(e);
        } else if (ch >= 0xa0) {         // Latin-1: the letters a Swedish layout gives
            insert(e, (char)ch);
            redraw(e);
        }
    }
}
