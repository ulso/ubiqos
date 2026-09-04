// argv and randomness, the two things a program gets from the host before it
// has done anything of its own. Rust needs both to start at all: its hash maps
// are seeded from random_get, and its runtime asks for the argument vector.
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("argrand: argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("argrand:   argv[%d] = %s\n", i, argv[i]);

    unsigned char b[16];
    if (getentropy(b, sizeof b) != 0) { printf("argrand: no entropy\n"); return 1; }

    printf("argrand: 16 random bytes:");
    for (unsigned i = 0; i < sizeof b; i++) printf(" %02x", b[i]);
    printf("\n");
    return 0;
}
