// Proof that a plain C program, built with clang against wasi-libc, reaches
// UbiqOS files through WASI. No argv: args_get is not implemented on the board.
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
    printf("ctest: hello from C on wasm\n");

    int fd = open("/sd/w4", O_RDONLY);
    if (fd < 0) { printf("ctest: cannot open /sd/w4\n"); return 1; }

    char b[64];
    int n = (int)read(fd, b, sizeof b - 1);
    if (n < 0) n = 0;
    b[n] = 0;
    printf("ctest: read %d bytes: %s\n", n, b);
    close(fd);
    return 0;
}
