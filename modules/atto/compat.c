// The two calls Atto wants that a UbiqOS module has no library answer for.
//
// This is the native twin of modules/wasm/examples/curses/compat.c, and the
// difference between them is two lines of directory reading. That file expands
// the pattern with opendir and readdir, which WASI has and which newlib on bare
// metal does not; here the same job is done with ubiqos_fs_dir_at, which asks
// the file server for the nth name and is what the shell's own ls uses.
//
// The wasm version also had to adopt PWD, because a guest started life in "/"
// and had to be told where it was standing. A native module has no such gap:
// the process already has a directory, so curses_adopt_pwd is empty here and
// kept only so that curses.c can call it without knowing which build it is in.
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../common/ubiqos_abi.h"

void curses_adopt_pwd(void) { }

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

// Filename completion on TAB, which Atto asks for by shelling out to
// "echo prefix* >tmpfile" and reading the names back.
//
// A module could exec a real shell here -- UbiqOS has one and SYS_EXEC to start
// it with. It does not, for the same reason the wasm side does not: the shell
// would have to glob, and it does not, so the work would land back here anyway
// having cost a process and a pipe. So this recognises the one command shape
// Atto sends, expands the pattern itself, and writes the names where the
// command would have put them. Atto is untouched and never learns.
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
        if (ubiqos_getcwd(dirname, sizeof dirname) < 0) strcpy(dirname, "/");
        strcpy(prefix, pattern);
    }
    unsigned plen = strlen(prefix);
    if (plen && prefix[plen - 1] == '*') prefix[--plen] = 0;

    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    // By index rather than through a handle, which is the shape UbiqOS gives a
    // directory: ask for the nth name until it says there is no nth name.
    char name[64];
    uint32_t size;
    int first = 1;
    for (uint32_t i = 0; ubiqos_fs_dir_at(dirname, i, name, &size) == 0; i++) {
        if (name[0] == '.') continue;
        if (plen && strncmp(name, prefix, plen) != 0) continue;
        if (!first) (void)!write(fd, " ", 1);
        first = 0;
        // The whole path, because that is what the shell would have echoed and
        // what Atto puts straight back into its prompt. A pattern with no slash
        // in it is answered with bare names, for the same reason.
        if (slash && dirname[1]) {
            (void)!write(fd, dirname, strlen(dirname));
            (void)!write(fd, "/", 1);
        } else if (slash) {
            (void)!write(fd, "/", 1);
        }
        (void)!write(fd, name, strlen(name));
    }
    close(fd);
    return 0;
}
