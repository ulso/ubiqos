// The two calls Atto wants that WASI has no answer for.
//
// Both are used by one feature: filename completion on TAB, which shells out to
// "echo prefix* >tmpfile" and reads the names back. A wasm guest cannot run a
// command, and it cannot list a directory either -- fd_readdir is not among the
// calls this host implements -- so the feature cannot work as written.
//
// It is made inert rather than removed. mkstemp really does make a file, in the
// /tmp this machine has, because a failed one calls Atto's fatal() and takes the
// editor down; system does nothing and says it worked, so the file is read, is
// empty, and TAB simply completes to nothing. An editor that does not complete
// is an editor; one that exits when you press TAB is not.
//
// Making it work would mean fd_readdir in modules/wasm/wasi.c and a glob here.
// Worth doing the day something else wants to read a directory too.
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

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

int system(const char *command)
{
    (void)command;
    return 0;              // "it ran, and produced nothing"
}
