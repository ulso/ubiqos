// The same job as modules/hibouair/hibouair.c, written against POSIX so it can
// be a file on the card instead of a module in flash. The BleuIO dongle is
// opened with open("/dev/acm", O_RDWR) and talked to with read and write: to a
// program up here it is a file, and an FTDI cable would be one too the day a
// driver registers it.
//
// The module waits with ubiqos_arm and a pulse. This waits by reading, because
// a device read on UbiqOS blocks until there is something -- so the whole of
// the arming, the message loop and the disarm come to one blocking call.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

// The dongle needs time between these. AT+CENTRAL changes the chip's role and
// AT+FINDSCANDATA starts a scan, and sending the three back to back gets the
// first echoed and the rest ignored -- which is exactly what happened before
// poll_oneoff existed on this host and this slept for nothing.
static void pause_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}

int main(void)
{
    int fd = open("/dev/acm", O_RDWR);
    if (fd < 0) { printf("hibou: no dongle at /dev/acm\n"); return 1; }

    static const char *setup[] = { "ATE0\r", "AT+CENTRAL\r", "AT+FINDSCANDATA=FF5B07\r" };
    for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        write(fd, setup[i], strlen(setup[i]));
        pause_ms(300);
    }

    printf("hibou: scanning for HibouAir sensors\n");

    char buf[256];
    for (;;) {
        int n = (int)read(fd, buf, sizeof buf);
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    close(fd);
    return 0;
}
