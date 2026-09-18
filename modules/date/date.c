#include "../../common/ubiqos_posix.h"

__thread int errno;

// date -- says what the clock thinks it is.
//
// The kernel keeps the clock and publishes it as /var/time, so this is that
// file read out: no system call of its own, no shell builtin, nothing the
// kernel had to be taught. Which is the point of a volume that holds what the
// kernel knows -- `cat /var/time` says exactly the same thing, and this exists
// because `date` is what a person types.
//
// Before the network has answered, /var/time reads "--------- --:--:--" and so
// does this. A clock that says it does not know is worth more than one that
// invents a year, and the board has no battery to remember one across a reset.
void module_main(void) {
    const int fd = open("/var/time", O_RDONLY);
    if (fd < 0) {
        ubiqos_write_str(UBIQOS_STDERR, "date: the kernel is not keeping a clock\n");
        return;
    }

    // One line, and a short one. It is generated at the moment of the read, so
    // a single call has all of it.
    char line[32];
    const int n = read(fd, line, sizeof line - 1);
    close(fd);

    if (n <= 0) {
        ubiqos_write_str(UBIQOS_STDERR, "date: the clock said nothing\n");
        return;
    }
    line[n] = 0;
    ubiqos_write_str(UBIQOS_STDOUT, line);
}
