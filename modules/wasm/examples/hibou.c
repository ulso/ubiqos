// The same job as modules/hibouair/hibouair.c, written against POSIX so it can
// be a wasm file instead of a myrtos module. The dongle is just a file.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

int main(void)
{
    int fd = open("/dev/acm", O_RDWR);
    if (fd < 0) { printf("no dongle\n"); return 1; }

    static const char *setup[] = { "ATE0\r", "AT+CENTRAL\r", "AT+FINDSCANDATA=FF5B07\r" };
    for (unsigned i = 0; i < sizeof(setup)/sizeof(setup[0]); i++)
        write(fd, setup[i], strlen(setup[i]));

    char buf[256];
    for (;;) {
        int n = (int)read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    close(fd);
    return 0;
}
