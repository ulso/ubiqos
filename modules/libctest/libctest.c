#include "../../common/myrtos_stdio.h"

// libctest -- checks the C library headers against what the standard says.
//
// It compares rather than prints, because eyeballing printf output is how a
// wrong width goes unnoticed for a year. Every line says ok or FAIL, and the
// last line is the count that matters.

MYRTOS_STDIO_DEFINE
MYRTOS_MEM_SIZE(8192);

// Thread-local, not plain static: a shareable module may not have writable
// data, and check_module.py refused this file until it said so. Per-process is
// also what a counter like this should be.
static __thread int failures;

static void check(const char *what, int ok)
{
    printf("  %-28s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void check_str(const char *what, const char *got, const char *want)
{
    int ok = !strcmp(got, want);
    printf("  %-28s %s", what, ok ? "ok" : "FAIL");
    if (!ok) printf("  got \"%s\" want \"%s\"", got, want);
    printf("\n");
    if (!ok) failures++;
}

void module_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[64];

    printf("printf\n");
    snprintf(b, sizeof b, "%d %d %u", 42, -42, 42u);
    check_str("integers", b, "42 -42 42");
    snprintf(b, sizeof b, "[%5d][%-5d][%05d]", 42, 42, 42);
    check_str("width and padding", b, "[   42][42   ][00042]");
    snprintf(b, sizeof b, "%x %X %o %%", 48879u, 48879u, 8u);
    check_str("bases", b, "beef BEEF 10 %");
    snprintf(b, sizeof b, "[%s][%.3s][%8s]", "abc", "abcdef", "abc");
    check_str("strings", b, "[abc][abc][     abc]");
    snprintf(b, sizeof b, "%c%c", 'h', 'i');
    check_str("characters", b, "hi");

    // The standard's snprintf: truncate, terminate, and return the length it
    // would have needed. Getting the return value wrong is the usual bug.
    int n = snprintf(b, 5, "abcdefgh");
    check("snprintf truncates", !strcmp(b, "abcd"));
    check("snprintf returns full length", n == 8);

    printf("sscanf\n");
    int a = 0, c = 0;
    check("two integers", sscanf("12 34", "%d %d", &a, &c) == 2 && a == 12 && c == 34);
    a = 0;
    check("hexadecimal", sscanf("ff", "%x", &a) == 1 && a == 255);
    a = 0;
    check("sign", sscanf("-7", "%d", &a) == 1 && a == -7);
    char word[16];
    check("string and number",
          sscanf("  hello 5", "%s %d", word, &a) == 2 && !strcmp(word, "hello") && a == 5);
    check("no match returns count", sscanf("xyz", "%d", &a) == 0);

    printf("string and ctype\n");
    check("strcmp orders", strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0);
    check("strstr finds", strstr("hello world", "o w") != 0);
    check("strstr misses", strstr("hello", "xyz") == 0);
    strcpy(b, "abc"); strcat(b, "def");
    check_str("strcat", b, "abcdef");
    check("strchr", strchr("abcabc", 'b')[0] == 'b');
    check("strrchr finds the last", strrchr("abcabc", 'b') == strchr("abcabc", 'b') + 3);
    check("ctype", isdigit('7') && !isdigit('x') && toupper('q') == 'Q' && isspace('\t'));
    check("strtol base 0", strtol("0x1f", 0, 0) == 31 && strtol("012", 0, 0) == 10);

    printf("malloc\n");
    char *p = (char *)malloc(16);
    check("malloc gives memory", p != 0);
    if (p) {
        strcpy(p, "before realloc");
        char *q = (char *)realloc(p, 64);
        check("realloc keeps the contents", q && !strcmp(q, "before realloc"));
        free(q);
    }
    int *z = (int *)calloc(8, sizeof(int));
    check("calloc zeroes", z && !z[0] && !z[7]);
    free(z);

    printf("%d failure%s\n", failures, failures == 1 ? "" : "s");
}
