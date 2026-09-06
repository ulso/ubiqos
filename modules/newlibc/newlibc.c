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
    printf("newlibc: the C library, not the subset\n");

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

    printf(failures ? "newlibc: FAILED\n" : "newlibc: passed\n");
    return failures ? 1 : 0;
}
