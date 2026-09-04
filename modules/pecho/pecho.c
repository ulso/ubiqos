#include "../../common/myrtos_abi.h"
#include "../../common/myrtos_stdio.h"

MYRTOS_LIBC_DEFINE

// pecho -- echo, but through printf.
//
// It exists to answer one question: does output redirected to a file survive
// when it goes through myrtos_stdio rather than straight down a write syscall?
// echo uses myrtos_write_str and its output reaches a file; hibouair uses printf
// and its output reaches nothing at all. This is the same job as echo with the
// one difference that matters.
//
//     pecho hello              to the console
//     pecho hello > /tmp/f     to a file
//     pecho -n 40 hello        forty lines, to fill and flush the 512-byte buffer
void module_main(int argc, char **argv)
{
    int arg = 1;
    unsigned long times = 1;

    // -o opens a device first, the way hibouair opens the dongle before it
    // prints anything. That is the one structural difference between this and
    // hibouair, whose redirected output reaches no file at all.
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'o' && !argv[1][2]) {
        int32_t dev = myrtos_open("/dev/acm");
        printf("[opened /dev/acm as fd %ld]\n", (long)dev);
        arg = 2;
    }

    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'n' && !argv[1][2]) {
        times = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            times = times * 10 + (unsigned long)(*p - '0');
        if (!times) times = 1;
        arg = 3;
    }

    for (unsigned long n = 0; n < times; n++) {
        for (int i = arg; i < argc; i++)
            printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
        printf(" [line %lu]\n", n);
    }
}
