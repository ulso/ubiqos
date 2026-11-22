// The three calls Atto wants that WASI has no answer for.
//
// Two of them belong to one feature: filename completion on TAB, which shells
// out to "echo prefix* >tmpfile" and reads the names back. A wasm guest cannot
// run a command -- there is no shell down there to run it in.
//
// So system does the job itself. It is not a shell and does not pretend to be
// one: it recognises the single command shape Atto sends, expands the pattern by
// reading the directory, and writes the names where the command would have put
// them. Atto is untouched and never learns the difference.
//
// That works because the host implements fd_readdir, which is the call a
// directory listing needs. It was added for this.
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <poll.h>
#include <time.h>

// The directory the program was started in, so that getcwd tells the truth.
//
// Opening a file no longer needs this: the host resolves a name that is not a
// volume against the process's directory, which is the only place it can be
// done for every guest at once. What still needs it is a program that asks
// where it is standing -- system() below expands a bare pattern in getcwd's
// answer, and without this it would answer "/".
//
// initscr calls it. As a constructor it did not take effect, and something a
// program depends on is worth an explicit call rather than a guess about when
// the runtime gets round to it.
// How big is the terminal? Ask it, as the native compat.c does: ESC [ 18 t,
// answered ESC [ 8 ; rows ; cols t by the board's console and by any real
// terminal at the far end of SSH. poll() is what waits a little for a reply
// that may never come; it reaches the host as poll_oneoff, which watches stdin
// since 22 Nov 2026 -- before that it answered "ready" without looking, the
// read after it blocked for ever, and this function had to give up and leave
// LINES and COLUMNS to the environment.
// A guest has no way to reach the foreground; it gets what the wasm host
// decides. Left as it was.
void curses_take_interrupt(void) { }

int curses_term_size(int *rows, int *cols)
{
    static const char q[] = "\x1b[18t";
    if (write(1, q, sizeof q - 1) != (ssize_t)(sizeof q - 1)) return 0;

    // 400 ms in all, as natively: the answer may cross SSH and a pipe pair. A
    // late one is not lost -- getch recognises it and eats it.
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int state = 0;                       // 0 outside, 1 after ESC, 2 parameters
    unsigned p[3] = { 0, 0, 0 };
    int np = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = 400 - ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return 0;
        struct pollfd pfd = { .fd = 0, .events = POLLIN };
        if (poll(&pfd, 1, (int)left) <= 0) return 0;
        unsigned char ch;
        if (read(0, &ch, 1) != 1) return 0;

        // The whole reply is consumed, terminator and all, or the rest turns up
        // as text on the next prompt.
        if (state == 0) {
            if (ch == 0x1b) { state = 1; np = 0; p[0] = p[1] = p[2] = 0; }
            continue;
        }
        if (state == 1) { state = (ch == '[') ? 2 : 0; continue; }
        if (ch >= '0' && ch <= '9') { if (np < 3) p[np] = p[np] * 10 + (unsigned)(ch - '0'); continue; }
        if (ch == ';') { if (np < 2) np++; continue; }
        if (ch == 't' && p[0] == 8 && np >= 2) {
            *rows = (int)p[1];
            *cols = (int)p[2];
            return 1;
        }
        state = 0;                       // some other sequence; keep waiting
    }
}

void curses_adopt_pwd(void)
{
    const char *p = getenv("PWD");
    if (p && *p) (void)!chdir(p);
}

// A real temporary file, because a failed mkstemp calls Atto's fatal() and
// takes the editor down with it.
int mkstemp(char *tmpl)
{
    static unsigned seq;
    unsigned n = strlen(tmpl);
    if (n < 6) return -1;

    // The six X's at the end become digits, as mkstemp promises.
    unsigned v = ++seq;
    for (unsigned i = 0; i < 6; i++) {
        tmpl[n - 1 - i] = (char)('0' + v % 10);
        v /= 10;
    }
    return open(tmpl, O_CREAT | O_RDWR | O_TRUNC, 0600);
}

// The rest of s after prefix, or nothing if it does not start with it.
static const char *after(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return s;
}

int system(const char *command)
{
    const char *p = after(command, "echo ");
    if (!p) return -1;

    // The pattern runs to " >", and the file name from there to the next space.
    char pattern[128], target[128];
    unsigned n = 0;
    while (*p && !(p[0] == ' ' && p[1] == '>') && n < sizeof pattern - 1) pattern[n++] = *p++;
    pattern[n] = 0;
    if (!*p) return -1;
    p += 2;                                    // past " >"
    n = 0;
    while (*p && *p != ' ' && n < sizeof target - 1) target[n++] = *p++;
    target[n] = 0;
    if (!n) return -1;

    // The directory is what stands before the last slash, the prefix what
    // follows it with any trailing star removed. "/sd/no*" is "/sd" and "no".
    char dirname[128], prefix[128];
    const char *slash = 0;
    for (const char *q = pattern; *q; q++) if (*q == '/') slash = q;
    if (slash) {
        unsigned d = (unsigned)(slash - pattern);
        if (!d) d = 1;                          // "/x" lists the root itself
        for (unsigned i = 0; i < d && i < sizeof dirname - 1; i++) dirname[i] = pattern[i];
        dirname[d < sizeof dirname ? d : sizeof dirname - 1] = 0;
        strcpy(prefix, slash + 1);
    } else {
        // No slash: the directory the program is standing in, which is the one
        // PWD put it in above.
        if (!getcwd(dirname, sizeof dirname)) strcpy(dirname, "/");
        strcpy(prefix, pattern);
    }
    unsigned plen = strlen(prefix);
    if (plen && prefix[plen - 1] == '*') prefix[--plen] = 0;

    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    DIR *d = opendir(dirname);
    if (d) {
        struct dirent *e;
        int first = 1;
        while ((e = readdir(d)) != 0) {
            if (e->d_name[0] == '.') continue;              // "." and ".."
            if (plen && strncmp(e->d_name, prefix, plen) != 0) continue;
            if (!first) (void)!write(fd, " ", 1);
            first = 0;
            // The whole path, because that is what the shell would have echoed
            // and what Atto puts straight back into its prompt. A pattern with
            // no slash in it is answered with bare names, for the same reason.
            if (slash && dirname[1]) {
                (void)!write(fd, dirname, strlen(dirname));
                (void)!write(fd, "/", 1);
            } else if (slash) {
                (void)!write(fd, "/", 1);
            }
            (void)!write(fd, e->d_name, strlen(e->d_name));
        }
        closedir(d);
    }
    close(fd);
    return 0;
}
