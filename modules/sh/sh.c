#include "../../common/ubiqos_abi.h"

// The UbiqOS shell. OS-9's shell did essentially this: read a name, look the
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

    // A shell reading a script is the same loop with the screen turned off: no
    // banner, no prompt, no echo of what it is about to run. What the commands
    // themselves print still goes out, because that is the point of running it.
    bool     quiet;
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
    ubiqos_getcwd(cwd, sizeof(cwd));
    uint32_t n = copy_str(e->prompt, "ubiqos:", PROMPT_MAX);
    n += copy_str(e->prompt + n, cwd[0] ? cwd : "/", PROMPT_MAX - n);
    n += copy_str(e->prompt + n, "> ", PROMPT_MAX - n);
    e->prompt_len = n;
}

// Everything from column one again: the prompt, the line, then erase to the end
// of it in case what was there before was longer, then put the cursor where it
// belongs. Absolute column addressing rather than a count of moves, so a
// miscount cannot accumulate.
static uint32_t view_width(editor_t *e);
static uint32_t view_start(editor_t *e);
static uint32_t columns_to(editor_t *e, uint32_t at);
static uint32_t byte_of_column(editor_t *e, uint32_t col);

static void redraw(editor_t *e) {
    if (e->quiet) return;            // a script has nobody to draw for
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "\r");
    ubiqos_line_str(&l, e->prompt);
    ubiqos_line_flush(e->out, &l);

    uint32_t start = view_start(e), w = view_width(e);
    uint32_t shown = byte_of_column(e, columns_to(e, start) + w) - start;
    if (shown) ubiqos_write(e->out, e->line + start, shown);

    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "\x1b[K\r\x1b[");
    ubiqos_line_u32(&l, e->prompt_len + (columns_to(e, e->pos) - columns_to(e, start)) + 1);
    ubiqos_line_str(&l, "G");
    ubiqos_line_flush(e->out, &l);
}

// How much of a command fits: the buffer, and nothing else.
//
// This used to be the width of the screen less the prompt. A line that runs past
// the right edge wraps, and then the carriage return in redraw comes back to the
// start of the wrapped part rather than the start of the line -- so rather than
// draw it wrong the editor simply stopped taking characters, and the note here
// said that a long path needs a cd first.
//
// Fair enough while every name was 8.3. It stopped being fair the day the
// filesystem grew long names: "write /sd/a-long-filename.txt" and something to
// put in it passes eighty columns easily, and the line was then cut without a
// word -- the file was created, under the right name, holding half the text.
// Refusing to draw is worse than scrolling.
static uint32_t room(editor_t *e) {
    (void)e;
    return LINE_MAX - 1;
}

// What is on screen, and where it starts.
//
// The line scrolls sideways under a prompt that stays put, the way a narrow
// terminal has always done it. The window moves only when the cursor would
// otherwise leave it, so it stands still while a line is edited in the middle
// and follows only at the edges.
static uint32_t view_width(editor_t *e) {
    return (e->width > e->prompt_len + 1) ? e->width - e->prompt_len - 1 : 1;
}

// Bytes and columns stopped being the same thing the day the machine spoke
// UTF-8. A-ring is two bytes and one glyph, so a cursor placed by counting
// bytes stands one column too far right for every accented character before
// it, and a backspace that removes one byte leaves half a character behind.
// The line stays a byte buffer -- everything that runs a command wants bytes --
// and these three say where the characters are in it.
static bool utf8_cont(char c) { return ((unsigned char)c & 0xc0) == 0x80; }

static uint32_t columns_to(editor_t *e, uint32_t at) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < at && i < e->len; i++) if (!utf8_cont(e->line[i])) n++;
    return n;
}

// The byte at which the nth character begins, or the end of the line.
static uint32_t byte_of_column(editor_t *e, uint32_t col) {
    uint32_t i = 0;
    for (uint32_t n = 0; n < col && i < e->len; n++) {
        i++;
        while (i < e->len && utf8_cont(e->line[i])) i++;
    }
    return i;
}

static uint32_t view_start(editor_t *e) {
    uint32_t w = view_width(e);
    uint32_t cur = columns_to(e, e->pos);
    return (cur < w) ? 0 : byte_of_column(e, cur - w + 1);
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
    // The carriage return goes in the SAME write as the probe, and that is the
    // whole of it. The reply is produced when the sequence is parsed, not when
    // this reads it, so returning the cursor immediately still reports 79 --
    // and it closes the window in which the cursor sits parked at the right
    // edge waiting for an answer.
    //
    // That window was milliseconds wide and it was being hit. At boot the USB
    // host task announces devices while this runs, its line began at column 79
    // because that is where the cursor was, and one character was left behind
    // at the edge with the rest wrapped onto the next row -- the "U" of "USB
    // host:", every time the two happened to coincide. A console byte trace is
    // what showed it; see vidstat -t.
    //
    // One write, because ubiqos_console_put takes a whole call into the ring
    // with interrupts off. Split across two, another writer could still land
    // between them.
    ubiqos_write_str(out, "\x1b[999C\x1b[6n\r");

    uint32_t deadline = ubiqos_ticks_now() + WIDTH_WAIT_MS;
    uint32_t col = 0, seen = 0;
    int state = 0;                       // 0 esc, 1 '[', 2 row, 3 col
    while ((int32_t)(ubiqos_ticks_now() - deadline) < 0) {
        uint8_t ch;
        // Ask before reading. ubiqos_read blocks when there is nothing, so
        // reading blindly here waited for the answer that was never coming --
        // until the first keystroke arrived, which it then ate. That is where
        // "echo one" came back as "cho one".
        if (ubiqos_readable(in) <= 0) { ubiqos_sleep(2); continue; }
        if (ubiqos_read(in, &ch, 1) <= 0) continue;
        if (state == 0) { if (ch == 0x1b) state = 1; }
        else if (state == 1) { state = (ch == '[') ? 2 : 0; }
        else if (state == 2) { if (ch == ';') state = 3; else if (ch < '0' || ch > '9') state = 0; }
        else {
            if (ch >= '0' && ch <= '9') { col = col * 10 + (uint32_t)(ch - '0'); seen = 1; }
            else { if (ch == 'R' && seen) break; state = 0; col = 0; seen = 0; }
        }
    }
    ubiqos_write_str(out, "\r");
    return (seen && col >= 20 && col <= 400) ? col : WIDTH_DEFAULT;
}

// --- THE LOOP -------------------------------------------------------------

static bool line_is(const char *line, const char *word) {
    while (*word) { if (*line != *word) return false; line++; word++; }
    return *line == 0;
}

// What start_one returns for a built in: it has already run, in this process,
// so there is no pid to wait for and nothing failed. Every caller that reads a
// negative return has to know this one, which is why it is not just -4.
#define SH_BUILTIN (-4)

// cd is built in and has to be. A module changing its own current directory
// changes nothing for the shell that started it -- the child gets a copy at
// exec and takes it to the grave.
static void change_dir(int32_t c, const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) arg = "/";

    if (ubiqos_chdir(arg) == 0) return;

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "cd: no such directory: ");
    ubiqos_line_str(&l, arg);
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_flush(c, &l);
}

static void help(int32_t c) {
    ubiqos_write_str(c,
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
        "  date     what the clock says, once the network has told it\r\n"
        "  mkdir    make a directory\r\n"
        "  rmdir    remove an empty directory\r\n"
        "  mount    take the SD card again\r\n"
        "  font     screen font, or 'font 6x12' to change it\r\n"
        "  color    text and background, or 'color' to see them\r\n"
        "  ehrpc    the radio: up, mode, connect <ssid>, peek\r\n"
        "  ehstat   what the link to the radio has carried\r\n"
        "  i2c      scan the bus, or read and write a register\r\n"
        "  tone     a sine out of the headphone jack\r\n"
        "  volume   0 to 100, or 'volume' to see it\r\n"
        "  play     a WAV file; 'play -i' says what one is without playing\r\n"
        "  adc      the analogue inputs; 'adc -i' reports its interrupt\r\n"
        "  gpio     the digital pins; 'gpio watch N' waits for a button\r\n"
        "  crit     hold a kernel critical section, to measure what that costs\r\n"
        "  kill     end a process by number\r\n"
        "  cu       talk to a device, e.g. 'cu /dev/acm' for a serial dongle\r\n"
        "  cat      show a file\r\n"
        "  more     a screenful at a time; space, Return, q\r\n"
        "  cp       copy a file\r\n"
        "  rm       delete a file\r\n"
        "  mv       rename a file, or move it within a volume\r\n"
        "  write    write text to a file\r\n"
        "  sleep    wait, in milliseconds\r\n"
        "  nice     run a command at a priority\r\n"
        "  usbdisk  hand the SD card to the host over USB\r\n"
        "  reboot   start the machine again\r\n"
        "  bootsel  reboot into the bootloader\r\n"
        "\r\nA trailing & runs a command without waiting for it.\r\n"
        "Arrows move along the line and up and down the history; ctrl-A and\r\n"
        "ctrl-E jump to its ends; ctrl-L clears the screen when it is empty.\r\n"
        "ctrl-C ends the running command, or abandons the line if none is.\r\n");
}

// --- REDIRECTION -----------------------------------------------------------
// The shell puts the file on the descriptor, starts the child, and puts its own
// descriptor back -- which is what every shell has done since the seventh
// edition, and what dup2 is for. The child needs to know nothing: it inherits
// numbered paths and writes to 1 as it always did.
typedef struct {
    int32_t fd;
    char    name[40];
    bool    append;
} redirect_t;

static bool token_is(const char *t, const char *w) {
    while (*w && *t == *w) { t++; w++; }
    return !*w && (!*t || *t == ' ');
}

// Take the redirections out of the command line, blanking them with spaces so
// what reaches the child is only its own arguments. The name is COPIED rather
// than terminated in place: a NUL in the middle of the line would cut off every
// argument after it, which is how "cmd > file arg" would quietly lose arg.
static int take_redirects(char *args, redirect_t *out, int max) {
    int n = 0;
    char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        int32_t fd = -1;
        bool append = false;
        int len = 0;
        if (token_is(p, "2>"))      { fd = UBIQOS_STDERR; len = 2; }
        else if (token_is(p, ">>")) { fd = UBIQOS_STDOUT; len = 2; append = true; }
        else if (token_is(p, ">"))  { fd = UBIQOS_STDOUT; len = 1; }
        else if (token_is(p, "<"))  { fd = UBIQOS_STDIN;  len = 1; }
        if (fd < 0) {                                   // an ordinary argument
            while (*p && *p != ' ') p++;
            continue;
        }

        char *tok = p;
        p += len;
        while (*p == ' ') p++;
        char *name = p;
        while (*p && *p != ' ') p++;

        if (name != p && n < max) {
            uint32_t i = 0;
            while (name + i < p && i < sizeof out[0].name - 1) {
                out[n].name[i] = name[i];
                i++;
            }
            out[n].name[i] = 0;
            out[n].fd = fd;
            out[n].append = append;
            n++;
        }
        while (tok < p) *tok++ = ' ';                   // the arrow and the name
    }
    return n;
}

// One command, with its redirections applied and undone, started but not
// waited for. A pipeline needs both halves running at once, so waiting cannot
// happen in here.
static int32_t start_one(char *cmd) {
    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = 0; args++; while (*args == ' ') args++; }

    redirect_t rd[3];
    int nrd = take_redirects(args, rd, 3);

    // Saved descriptors, so the shell's own 0, 1 and 2 come back afterwards.
    int32_t saved[3] = { -1, -1, -1 };
    int opened = 0;
    for (int i = 0; i < nrd; i++) {
        // The arrow says what the flags are, and the kernel does the rest:
        // emptying the file for >, placing the descriptor at the end for >>,
        // and refusing a missing one for <.
        uint32_t flags = rd[i].fd == UBIQOS_STDIN
                       ? UBIQOS_O_RDONLY
                       : UBIQOS_O_WRONLY | UBIQOS_O_CREAT
                         | (rd[i].append ? UBIQOS_O_APPEND : UBIQOS_O_TRUNC);

        int32_t f = ubiqos_open_flags(rd[i].name, flags);
        if (f < 0) {
            ubiqos_write_str(UBIQOS_STDERR, "sh: cannot open the file\n");
            break;                                      // the command does not run
        }
        saved[i] = ubiqos_dup(rd[i].fd, -1);
        ubiqos_dup(f, rd[i].fd);
        ubiqos_close(f);
        opened++;
    }

    // The built ins belong here rather than in the reader loop, because here
    // they are behind the same redirections as everything else. Dispatching
    // them on the whole line meant `help | more` never reached this function
    // at all: the pipe split the line first, and `help` was then looked for
    // among the modules and not found. They write to UBIQOS_STDOUT, which is
    // the pipe's file, or the > file, whenever one of those is in place.
    //
    // -3 rather than -1: the redirection said what was wrong, and the caller
    // adding "no such module" to it sends the reader looking for the wrong
    // thing entirely.
    int32_t pid;
    if (opened != nrd) {
        pid = -3;
    } else if (line_is(cmd, "help")) {
        help(UBIQOS_STDOUT);
        pid = SH_BUILTIN;
    } else if (line_is(cmd, "cd")) {
        change_dir(UBIQOS_STDOUT, args);
        pid = SH_BUILTIN;
    } else {
        pid = ubiqos_exec(cmd, args);
    }

    // Back to the terminal. The child took its copy when it was made, so this
    // cannot reach it.
    for (int i = nrd - 1; i >= 0; i--) {
        if (saved[i] < 0) continue;
        ubiqos_dup(saved[i], rd[i].fd);
        ubiqos_close(saved[i]);
    }
    return pid;
}

// left | right, through a file in PSRAM rather than a buffer with two ends.
//
// Ulf's idea, and the better one for this machine. The kernel has a real pipe --
// ubiqos_pipe, blocking, with end of file once the writers have gone -- and
// driving it from the shell hung twice, because a child inherits EVERY
// descriptor its parent holds and there is no fork here in which to close what
// it does not need. Through /tmp there is nothing to inherit: the halves run one
// after the other, and each is only the redirection that already works.
//
// The price is that it does not stream. All of the left side exists before the
// right side starts, so `big | head` writes the whole of big first. With eight
// megabytes of PSRAM and no `yes` to run for ever that is a fair trade, and the
// streaming version is an upgrade of this shape rather than a different one.
static int32_t run_between(char *cmd, int32_t fd, const char *file, uint32_t flags) {
    int32_t f = ubiqos_open_flags(file, flags);
    if (f < 0) {
        ubiqos_write_str(UBIQOS_STDERR, "sh: no room in /tmp for the pipe\n");
        return -3;
    }
    int32_t saved = ubiqos_dup(fd, -1);
    ubiqos_dup(f, fd);
    ubiqos_close(f);

    int32_t pid = start_one(cmd);
    if (pid >= 0) {
        ubiqos_foreground(UBIQOS_STDIN, pid);
        ubiqos_wait(pid);
        ubiqos_foreground(UBIQOS_STDIN, 0);
    }

    ubiqos_dup(saved, fd);
    ubiqos_close(saved);
    return pid;
}

// Which command could not be started, for the caller to name. It is set for
// every failure that has a name to give, and read only when exec_line returns
// one of those -- see the note where it is reported.
static const char *failed_name;

static int32_t run_pipeline(char *left, char *right) {
    const char *between = "/tmp/pipe";

    int32_t p1 = run_between(left, UBIQOS_STDOUT, between,
                             UBIQOS_O_WRONLY | UBIQOS_O_CREAT | UBIQOS_O_TRUNC);
    if (p1 == -3) return -3;
    if (p1 < 0 && p1 != SH_BUILTIN) {
        ubiqos_fs_remove(between); failed_name = left; return p1;
    }

    int32_t p2 = run_between(right, UBIQOS_STDIN, between, UBIQOS_O_RDONLY);
    ubiqos_fs_remove(between);
    // The half that failed is the half to name. start_one has NUL-terminated
    // each of these at its first space, so both are bare command names by now.
    if (p2 < 0 && p2 != SH_BUILTIN) failed_name = right;
    return p2;
}

// Split the line at the first space: everything before is the module name,
// everything after is the command line the process is given.
// --- COMPLETION ------------------------------------------------------------
// Tab fills in what can only be one thing, and stops where it becomes a choice.
//
// The first word is a command, so the candidates are the module directory; any
// later word is a path, so they are the names in the directory it points at.
// Both are the same problem once the candidates can be walked, which is why
// there is one loop and a flag rather than two of everything.
//
// It completes to the longest common prefix rather than to the first match. A
// completion that guesses is worse than one that stops: stopping tells you
// exactly how far the name is decided, and typing one more letter asks again.

// Where the word under the cursor starts. Everything is split on spaces here,
// as the rest of this shell does -- quoting does not exist yet, and inventing
// it in the completer alone would make Tab disagree with Return.
static uint32_t word_start(editor_t *e)
{
    uint32_t i = e->pos;
    while (i && e->line[i - 1] != ' ') i--;
    return i;
}

static bool first_word(editor_t *e, uint32_t start)
{
    for (uint32_t i = 0; i < start; i++)
        if (e->line[i] != ' ') return false;
    return true;
}

// How much of a and b agree, in bytes.
static uint32_t common(const char *a, const char *b)
{
    uint32_t n = 0;
    while (a[n] && a[n] == b[n]) n++;
    return n;
}

// Insert text at the cursor, which is what every completion ends up doing.
static void insert_str(editor_t *e, const char *s)
{
    for (uint32_t i = 0; s[i]; i++) insert(e, s[i]);
}

// Split a path into the directory to look in and the fragment to match. "/sd/do"
// looks in "/sd" for "do"; "do" looks in the current directory for "do".
static void split_path(const char *word, char *dir, uint32_t dir_max, const char **leaf)
{
    uint32_t cut = 0, n = 0;
    for (; word[n]; n++) if (word[n] == '/') cut = n + 1;
    *leaf = word + cut;
    if (!cut) { dir[0] = '.'; dir[1] = 0; return; }
    uint32_t keep = cut > 1 ? cut - 1 : 1;          // "/x" keeps the root's slash
    if (keep >= dir_max) keep = dir_max - 1;
    for (uint32_t i = 0; i < keep; i++) dir[i] = word[i];
    dir[keep] = 0;
}

static void complete(editor_t *e)
{
    uint32_t start = word_start(e);
    char frag[LINE_MAX];
    uint32_t flen = e->pos - start;
    for (uint32_t i = 0; i < flen; i++) frag[i] = e->line[start + i];
    frag[flen] = 0;

    bool commands = first_word(e, start);
    char dir[128]; const char *leaf = frag;
    if (!commands) split_path(frag, dir, sizeof dir, &leaf);
    uint32_t leaf_len = 0;
    while (leaf[leaf_len]) leaf_len++;

    char best[UBIQOS_DIRNAME_MAX];
    uint32_t best_len = 0, matches = 0;

    for (uint32_t i = 0; ; i++) {
        char name[UBIQOS_DIRNAME_MAX];
        if (commands) {
            ubiqos_modinfo_t m;
            if (ubiqos_moddir_get(i, &m) < 0) break;
            // A data module is a descriptor or a keymap, not something to run.
            if (m.type == UBIQOS_TYPE_DATA) continue;
            uint32_t k = 0;
            while (k < UBIQOS_NAME_LEN - 1 && m.name[k]) { name[k] = m.name[k]; k++; }
            name[k] = 0;
        } else {
            char raw[UBIQOS_DIRNAME_MAX];
            uint32_t size = 0;
            if (ubiqos_fs_dir_at(dir, i, raw, &size) < 0) break;
            ubiqos_pretty_name(raw, name);
        }

        bool hit = true;
        for (uint32_t k = 0; k < leaf_len; k++) if (name[k] != leaf[k]) { hit = false; break; }
        if (!hit) continue;

        if (!matches) {
            uint32_t k = 0;
            while (name[k]) { best[k] = name[k]; k++; }
            best[k] = 0; best_len = k;
        } else {
            best_len = common(best, name);
            best[best_len] = 0;
        }
        matches++;
    }

    if (!matches) return;                 // nothing to say, and nothing said

    if (best_len > leaf_len) {
        insert_str(e, best + leaf_len);
        // A single match is finished, so the space that would be typed next is
        // typed here. More than one is not: the cursor stops exactly where the
        // name stopped being decided.
        if (matches == 1) insert_str(e, " ");
        redraw(e);
        return;
    }

    // The prefix is already as long as it can get, so the only useful thing
    // left is to show what the choices are. Printed above a fresh prompt, so
    // the line being edited is not lost.
    if (matches > 1) {
        ubiqos_write_str(e->out, "\r\n");
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        uint32_t shown = 0;
        for (uint32_t i = 0; ; i++) {
            char name[UBIQOS_DIRNAME_MAX];
            if (commands) {
                ubiqos_modinfo_t m;
                if (ubiqos_moddir_get(i, &m) < 0) break;
                if (m.type == UBIQOS_TYPE_DATA) continue;
                uint32_t k = 0;
                while (k < UBIQOS_NAME_LEN - 1 && m.name[k]) { name[k] = m.name[k]; k++; }
                name[k] = 0;
            } else {
                char raw[UBIQOS_DIRNAME_MAX];
                uint32_t size = 0;
                if (ubiqos_fs_dir_at(dir, i, raw, &size) < 0) break;
                ubiqos_pretty_name(raw, name);
            }
            bool hit = true;
            for (uint32_t k = 0; k < leaf_len; k++) if (name[k] != leaf[k]) { hit = false; break; }
            if (!hit) continue;
            ubiqos_line_str(&l, name);
            ubiqos_line_str(&l, "  ");
            if (++shown % 6 == 0) { ubiqos_line_str(&l, "\r\n"); ubiqos_line_flush(e->out, &l); ubiqos_line_reset(&l); }
        }
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(e->out, &l);
        redraw(e);
    }
}

static int32_t exec_line(char *line) {
    // Whatever fails, it is this unless a pipeline says otherwise. A single
    // command's name is the head of the line, which start_one terminates.
    failed_name = line;

    // A pipe splits the line before anything else looks at it, because each
    // half has its own name, arguments and redirections.
    for (char *b = line; *b; b++) {
        if (*b != '|') continue;
        *b = 0;
        char *right = b + 1;
        while (*right == ' ') right++;
        char *e = b;
        while (e > line && e[-1] == ' ') *--e = 0;
        if (!*line || !*right) {
            ubiqos_write_str(UBIQOS_STDERR, "sh: a pipe wants a command on both sides\n");
            return -3;
        }
        return run_pipeline(line, right);
    }


    // A trailing & means do not wait. Without it there is no way to have two
    // processes running at once from the keyboard, and no way to see that the
    // sleep list orders more than one sleeper.
    bool background = false;
    char *end = line;
    while (*end) end++;
    while (end > line && end[-1] == ' ') end--;
    if (end > line && end[-1] == '&') {
        background = true;
        end[-1] = 0;
        while (end > line && end[-1] == ' ') { end--; *end = 0; }
    }

    int32_t pid = start_one(line);
    // Wait for it before prompting again. Without this the prompt raced the
    // command's own output, and two commands in a row interleaved their lines.
    if (pid >= 0 && !background) {
        // Name it as the one ctrl-C should end. The kernel cannot work that out
        // -- by then the process is usually blocked in a rendezvous, reading
        // nothing -- and the shell is the only thing that knows what it started
        // and on which terminal.
        ubiqos_foreground(UBIQOS_STDIN, pid);
        ubiqos_wait(pid);
        ubiqos_foreground(UBIQOS_STDIN, 0);
    }
    return pid;
}

void module_main(int argc, char **argv) {
    // The kernel has already given us 0, 1 and 2. The shell opens nothing.
    editor_t ed;
    editor_t *e = &ed;

    // "sh script" is how the boot script is run: same shell, stdin already
    // pointed at the file by whoever started us. It is told rather than
    // detected because the alternative is asking whether path 0 is a file,
    // and a shell that has to ask about its own descriptors is one syscall
    // away from caring where its input comes from, which it should not.
    e->quiet = argc > 1 && line_is(argv[1], "script");

    e->out = UBIQOS_STDOUT;
    e->len = e->pos = 0;
    e->line[0] = 0;
    e->stash[0] = 0;
    e->hist_count = e->hist_next = e->browse = 0;
    e->hist = (char*)ubiqos_alloc(HIST_LINES * LINE_MAX);

    if (!e->quiet) {
        ubiqos_write_str(e->out, "\r\nubiqos shell ready. Type 'help'.\r\n");
        // Not merely pointless on a script but destructive: ask_width writes a
        // cursor report request and then reads whatever comes back for 150 ms.
        // Pointed at a file it swallows the first 150 ms of the script looking
        // for an escape sequence that is never coming.
        e->width = ask_width(e->out, UBIQOS_STDIN);
    } else {
        e->width = WIDTH_DEFAULT;
    }
    build_prompt(e);
    redraw(e);

    int state = 0;                       // 0 text, 1 after ESC, 2 in CSI
    uint32_t csi_num = 0;

    for (;;) {
        uint8_t ch;
        int32_t got = ubiqos_read(UBIQOS_STDIN, &ch, 1);
        // Zero is the end and not "nothing yet": the kernel blocks a process
        // whose device has nothing to say rather than returning, so a nought
        // reaching here came from a file that has been read to its end or a
        // pipe whose writers have gone. An interactive shell never sees one,
        // and a script shell sees exactly one, at the bottom of the file.
        if (got == 0) break;
        if (got < 0) continue;

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
            case 'C':
                if (e->pos < e->len) {
                    e->pos++;
                    while (e->pos < e->len && utf8_cont(e->line[e->pos])) e->pos++;
                    redraw(e);
                }
                break;
            case 'D':
                if (e->pos) {
                    e->pos--;
                    while (e->pos && utf8_cont(e->line[e->pos])) e->pos--;
                    redraw(e);
                }
                break;
            case 'H': e->pos = 0;      redraw(e); break;
            case 'F': e->pos = e->len; redraw(e); break;
            case '~':
                if (csi_num == 3) {                                      // Delete
                    delete_at(e, e->pos);
                    while (e->pos < e->len && utf8_cont(e->line[e->pos])) delete_at(e, e->pos);
                    redraw(e);
                }
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
            if (!e->quiet) ubiqos_write_str(e->out, "\r\n");
            bool ran = e->len != 0;
            if (ran) {
                remember(e);
                int32_t r = exec_line(e->line);
                if (r < 0 && r != -3 && r != SH_BUILTIN) {
                    ubiqos_line_t l;
                    ubiqos_line_reset(&l);
                    // -2 means the module is there but is not re-entrant and
                    // is already running. Saying "no such module" for that
                    // sends the reader looking for the wrong problem.
                    ubiqos_line_str(&l, r == -2 ? "already running: "
                                                : "no such module: ");
                    // The command that failed, which for a pipeline is not
                    // the head of the line. This read e->line, and start_one
                    // NUL-terminates the LEFT half at its first space -- so
                    // `echo x | nosuch` reported "no such module: echo",
                    // naming the command that had just run successfully.
                    ubiqos_line_str(&l, failed_name);
                    ubiqos_line_str(&l, "\r\n");
                    ubiqos_line_flush(e->out, &l);
                }
            }
            e->len = e->pos = e->browse = 0;
            e->line[0] = 0;
            build_prompt(e);             // cd may have moved us
            if (e->quiet) continue;
            // A blank line between a command's output and the next prompt, as
            // there has always been. Nothing ran, nothing to separate.
            if (ran) ubiqos_write_str(e->out, "\r\n");
            redraw(e);
        } else if (ch == 3) {            // Ctrl-C, with nothing running
            // The kernel passes it through when there is no command to end, so
            // it does here what it does everywhere: abandon the line.
            ubiqos_write_str(e->out, "^C\r\n");
            e->len = e->pos = e->browse = 0;
            e->line[0] = 0;
            redraw(e);
        } else if (ch == 9) {            // Tab
            complete(e);
        } else if (ch == 1) {            // Ctrl-A
            e->pos = 0; redraw(e);
        } else if (ch == 5) {            // Ctrl-E
            e->pos = e->len; redraw(e);
        } else if (ch == 12) {           // Ctrl-L, but only on an empty line
            if (!e->len) { ubiqos_write_str(e->out, "\x1b[2J\x1b[H"); redraw(e); }
        } else if (ch == 8 || ch == 127) {
            // Back over a whole character: the continuation bytes first, then
            // the one that started it.
            if (e->pos) {
                bool more;
                do {
                    e->pos--;
                    more = utf8_cont(e->line[e->pos]);
                    delete_at(e, e->pos);
                } while (more && e->pos);
                redraw(e);
            }
        } else if (ch >= ' ' && ch < 0x7f) {
            insert(e, (char)ch);
            redraw(e);
        } else if (ch >= 0x80) {         // a byte of a UTF-8 character
            insert(e, (char)ch);
            redraw(e);
        }
    }
}
