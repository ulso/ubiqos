// wls -- ls, as a wasm program, over opendir and readdir.
//
// It exists to test fd_readdir on its own. Atto's TAB completion reads a
// directory through three layers -- its own completion code, the system() shim,
// and wasi-libc's dirent -- and when nothing comes back there is no way to tell
// which of them is silent. This is the same question with nothing else in it.
#include <stdio.h>
#include <dirent.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";

    DIR *d = opendir(path);
    if (!d) { printf("wls: cannot open %s\n", path); return 1; }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        printf("  %-24s type %d\n", e->d_name, (int)e->d_type);
        n++;
    }
    closedir(d);
    printf("wls: %d entries in %s\n", n, path);
    return 0;
}
