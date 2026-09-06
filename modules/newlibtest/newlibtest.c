// What a module gets when it is built NEWLIB: the standard library, rather than
// the header-only subset in common/myrtos_stdio.h.
//
// Everything here is deliberately something that subset does not have. qsort
// and bsearch are algorithms rather than syscalls; strdup needs malloc; fopen
// on a real file goes all the way down through _open, _read and _lseek in
// common/myrtos_syscalls.c to the file server. If this prints "passed" the
// bottom end is wired up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "../../common/myrtos_abi.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %-28s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static int by_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("newlibtest: the C library, not the subset\n");

    int v[] = { 42, 7, 99, 1, 13 };
    qsort(v, 5, sizeof v[0], by_int);
    check("qsort orders", v[0] == 1 && v[4] == 99);

    int key = 13;
    int *hit = bsearch(&key, v, 5, sizeof v[0], by_int);
    check("bsearch finds", hit && *hit == 13);

    char *dup = strdup("borrowed");
    check("strdup and malloc", dup && !strcmp(dup, "borrowed"));
    free(dup);

    char buf[32];
    snprintf(buf, sizeof buf, "%s/%d", "sd", 4);
    check("snprintf", !strcmp(buf, "sd/4"));

    // The whole way down: fopen, fgets, fclose against the file server.
    FILE *f = fopen("/sd/w4", "r");
    check("fopen a real file", f != NULL);
    if (f) {
        char line[16] = { 0 };
        char *got = fgets(line, sizeof line, f);
        check("fgets reads it", got && line[0] == 'h');
        fclose(f);
    }

    // Writing is the half that flag translation gets wrong, and the half a
    // read-only test cannot see: newlib's O_CREAT is myrtos's O_TRUNC, so a
    // straight pass-through asks to truncate a file it never creates.
    FILE *w = fopen("/sd/nlctest.txt", "w");
    check("fopen for writing", w != NULL);
    if (w) {
        fprintf(w, "%s %d\n", "written", 7);
        fclose(w);
        FILE *r = fopen("/sd/nlctest.txt", "r");
        char back[32] = { 0 };
        check("read back what was written",
              r && fgets(back, sizeof back, r) && !strcmp(back, "written 7\n"));
        if (r) fclose(r);
        remove("/sd/nlctest.txt");
    }

    // How nearly every C program asks how big a file is. myrtos could not
    // answer it at all until SEEK_END was sent to the file server: the trap
    // has no way to learn a length, and a cached one goes stale the moment
    // somebody writes.
    FILE *e = fopen("/sd/w4", "r");
    check("fopen for seeking", e != NULL);
    if (e) {
        check("fseek END then ftell", fseek(e, 0, SEEK_END) == 0 && ftell(e) == 4);
        check("fseek back to the start", fseek(e, 0, SEEK_SET) == 0 && ftell(e) == 0);
        fclose(e);
    }

    // Refused, not ignored: myrtos cannot promise the file did not exist.
    errno = 0;
    int x = open("/sd/w4", O_RDONLY | O_EXCL);
    check("O_EXCL is refused, not dropped", x < 0);
    if (x >= 0) close(x);

    // S_ISDIR, which is how anything that walks a tree decides to descend.
    struct stat sb;
    check("stat says /sd/docs is a directory",
          stat("/sd/docs", &sb) == 0 && S_ISDIR(sb.st_mode));
    check("stat says /sd/w4 is a file",
          stat("/sd/w4", &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size == 4);

    // Walking a directory, which newlib has no answer for at all -- its own
    // <dirent.h> is one #error. Anything that completes a filename or looks for
    // a config file does this on its first page.
    DIR *dir = opendir("/sd");
    check("opendir", dir != NULL);
    if (dir) {
        int entries = 0, saw_w4 = 0, saw_docs_as_dir = 0, zero_ino = 0;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            entries++;
            if (!de->d_ino) zero_ino = 1;
            if (!strcmp(de->d_name, "w4") && de->d_type == DT_REG) saw_w4 = 1;
            if (!strcmp(de->d_name, "docs") && de->d_type == DT_DIR) saw_docs_as_dir = 1;
        }
        check("readdir returns entries", entries > 5);
        check("d_type tells a file", saw_w4);
        check("d_type tells a directory", saw_docs_as_dir);
        check("d_ino is never zero", !zero_ino);

        rewinddir(dir);
        de = readdir(dir);
        check("rewinddir starts over", de != NULL);
        check("closedir", closedir(dir) == 0);
    }
    check("opendir of a file fails", opendir("/sd/w4") == NULL);
    check("opendir of nothing fails", opendir("/sd/nosuchdir") == NULL);

    // errno, which used to be ENOENT whatever went wrong. The distinction that
    // matters is "not there" against "there and it still did not open": a
    // program told ENOENT writes a default file, and being told it for a
    // directory would have it overwrite nothing at all, for ever.
    errno = 0;
    check("missing file gives ENOENT",
          fopen("/sd/nosuchfile.txt", "r") == NULL && errno == ENOENT);
    errno = 0;
    check("a directory gives EISDIR",
          open("/sd/docs", O_WRONLY) < 0 && errno == EISDIR);

    // Time. There is no clock on this board, so the epoch is boot -- which is
    // wrong for a date and right for measuring, which is what ports use it for.
    time_t t0 = time(NULL);
    clock_t c0 = clock();
    myrtos_sleep(1200);
    time_t t1 = time(NULL);
    clock_t c1 = clock();
    check("time advances over a sleep", t1 >= t0 + 1 && t1 <= t0 + 3);
    check("clock advances too", c1 > c0);

    printf(failures ? "newlibtest: FAILED\n" : "newlibtest: passed\n");
    return failures ? 1 : 0;
}
