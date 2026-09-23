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
// Nobody to ask here. A guest reaches its terminal through WASI, which has no
// way to wait a little for an answer that may never come -- and a blocking read
// of a cursor report that nothing will send hangs the editor. The environment
// is what this build goes by; see the native compat.c for the other half.
// A guest has no way to reach the foreground; it gets what the wasm host
// decides. Left as it was.
void curses_take_interrupt(void) { }

int curses_term_size(int *rows, int *cols)
{
    (void)rows; (void)cols;
    return 0;
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
