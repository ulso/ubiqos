// Host test for Blixt's assembler: assemble a file and compare the bytes with
// a reference binary made by GNU as (see run.sh). Prints each line whose bytes
// differ, with both encodings, and exits non-zero on any difference.
//
//   asmtest FILE.s REF.bin       compare
//   asmtest FILE.s               just assemble and list
#include "../asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **lines;
    int   *lens;
    int    n;
} src_t;

static const char *get_line(void *ctx, int i, int *len)
{
    src_t *s = ctx;
    *len = s->lens[i];
    return s->lines[i];
}

static char *slurp(const char *path, long *size);

static char *load_inc(void *ctx, const char *path, long *size)
{
    (void)ctx;
    return slurp(path, size);
}

static char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*size + 1);
    if (fread(b, 1, (size_t)*size, f) != (size_t)*size) { fclose(f); free(b); return 0; }
    b[*size] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: asmtest FILE.s [REF.bin]\n"); return 2; }
    long size;
    char *text = slurp(argv[1], &size);
    if (!text) { perror(argv[1]); return 2; }
    src_t s = { 0 };
    int cap = 0;
    char *p = text;
    while (*p) {
        char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (s.n == cap) {
            cap = cap ? cap * 2 : 256;
            s.lines = realloc(s.lines, (size_t)cap * sizeof *s.lines);
            s.lens = realloc(s.lens, (size_t)cap * sizeof *s.lens);
        }
        s.lines[s.n] = p;
        s.lens[s.n] = len;
        s.n++;
        if (!e) break;
        p = e + 1;
    }

    asm_result_t r;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", argv[1]);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else strcpy(dir, ".");
    asm_include_t inc = { load_inc, 0, dir };
    bool ok = asm_assemble(get_line, &s, s.n, &inc, &r);
    for (int i = 0; i < r.nerrors && i < ASM_MAX_ERRORS; i++)
        printf("%s:%d: %s\n", argv[1], r.errors[i].line + 1, r.errors[i].msg);
    if (!ok) return 1;

    if (argc < 3) {
        for (int i = 0; i < s.n; i++) {
            printf("%04x ", r.line_addr[i]);
            for (uint32_t k = 0; k < 6; k++)
                if (k < r.line_size[i]) printf("%02x", r.code[r.line_addr[i] + k]); else printf("  ");
            printf("  %.*s\n", s.lens[i], s.lines[i]);
        }
        printf("%u bytes, %u relocations, entry %u, %d passes\n", r.size, r.nrelocs, r.entry, r.passes);
        return 0;
    }

    long rsize;
    unsigned char *ref = (unsigned char *)slurp(argv[2], &rsize);
    if (!ref) { perror(argv[2]); return 2; }
    int bad = 0;
    for (int i = 0; i < s.n; i++) {
        uint32_t a = r.line_addr[i], n = r.line_size[i];
        if (!n) continue;
        bool same = a + n <= (uint32_t)rsize && memcmp(r.code + a, ref + a, n) == 0;
        if (same) continue;
        if (++bad > 40) continue;
        printf("line %d @%04x: blixt ", i + 1, a);
        for (uint32_t k = 0; k < n && k < 8; k++) printf("%02x", r.code[a + k]);
        printf("  gas ");
        for (uint32_t k = 0; k < n && k < 8 && a + k < (uint32_t)rsize; k++) printf("%02x", ref[a + k]);
        printf("   %.*s\n", s.lens[i], s.lines[i]);
    }
    // GNU as pads the end of the section to a word, with zeros and a nop;
    // that is not part of the program.
    if ((uint32_t)rsize > r.size && (uint32_t)rsize == ((r.size + 3) & ~3u)) {
        bool pad = true;
        for (long k = r.size; k < rsize; k++) if (ref[k] != 0x00 && ref[k] != 0xbf) pad = false;
        if (pad) rsize = r.size;
    }
    if (r.size != (uint32_t)rsize) {
        printf("size: blixt %u, gas %ld\n", r.size, rsize);
        if (!bad) {
            for (uint32_t k = 0; k < r.size && k < (uint32_t)rsize; k++)
                if (r.code[k] != ref[k]) { printf("first difference at %04x\n", k); break; }
        }
        bad++;
    } else if (!bad && memcmp(r.code, ref, (size_t)rsize) != 0) {
        for (uint32_t k = 0; k < r.size; k++)
            if (r.code[k] != ref[k]) { printf("pool or padding differs at %04x\n", k); break; }
        bad++;
    }
    printf("%s: %s (%u bytes, %d passes)\n", argv[1], bad ? "DIFFERENT" : "same as gas", r.size, r.passes);
    return bad ? 1 : 0;
}
