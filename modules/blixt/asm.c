// Blixt's assembler: Thumb-2 (ARMv8-M Mainline) in UAL syntax.
//
// Every instruction is encoded the way GNU as encodes it, because GNU as is
// what it is tested against, byte for byte: tools/blixt-asmtest assembles the
// same source with both and compares. Where two encodings are possible the
// shorter one is taken, as GNU as does, unless the mnemonic says .w.
//
// What a pass does: walk the lines, give each label the address it has now,
// and encode each instruction with what is known. The rules are GNU as's own
// (relaxed_symbol_addr and arm_relax_frag in tc-arm.c), because the tests hold
// Blixt to its output:
//
//   - A label ahead has the value the last pass gave it, moved on by as much as
//     this line has moved since -- less what an alignment in between would
//     swallow: the move is rounded down to each one's multiple.
//   - Each instruction takes the shortest form that fits what it sees, and may
//     shrink again, unless it is long while nothing before it has grown in
//     this pass. Then it is long for good, which is what makes the passes end.
//
// When a whole pass changes nothing, every choice in it was made on true
// values, and one more pass writes the code and the errors. STICKY_AFTER is a
// last resort that stops all shrinking.
#include "asm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PASSES 32
#define STICKY_AFTER 12
#define LINE_MAX_LEN 256

// --- values ----------------------------------------------------------------

// An expression's value. `rel` counts the labels in it: zero is a plain
// number, one is an address in the code, which is an offset until the loader
// adds where the code landed. `func` marks an address of a .thumb_func label,
// which gets its Thumb bit where it is stored as data.
typedef struct {
    int32_t v;
    int     rel;
    bool    undef;
    bool    func;
    bool    est;        // a label ahead, estimated from the last pass
} val_t;

typedef struct {
    char     name[32];
    int32_t  value;
    int      rel;
    bool     func;
    bool     equ;
    int      defpass;        // pass it was last defined in, -1 never
    int      line;
    int      pos;            // where in the order of lines it is defined
    int      next;           // hash chain
} sym_t;

typedef struct {
    int32_t v;
    int     rel;
    bool    func;
} lit_t;

typedef struct {
    asm_line_fn get;
    void       *ctx;
    int         nlines;
    asm_result_t *r;

    int      pass;
    bool     final;
    bool     changed;
    uint32_t addr;
    int      line;
    bool     line_failed;

    uint8_t *buf;
    uint32_t cap;

    uint32_t *rel;
    uint32_t  nrel, relcap;

    sym_t   *syms;
    int      nsyms, symcap;
    int      bucket[256];

    // Literal pools: every pool collects the `ldr rX, =` constants since the
    // one before. pool_addr remembers where each pool was put, from the last
    // pass, so a load can be encoded before its pool is reached.
    lit_t   *lit;
    int      nlit, litcap, pool_first;
    uint32_t *pool_addr;
    bool     *pool_known;
    int      npool_cap, pool_no;

    // The IT block being worked through.
    int      it_left;
    int      it_idx;
    uint8_t  it_cond[4];

    bool     next_func;
    bool     minwide;      // this line was 32 bits in an earlier pass
    bool     sticky;       // and is therefore kept at 32 (only after STICKY_AFTER)

    // Where a line is, for telling two definitions of a label apart: the
    // line number in the source, or for an included line its instance and
    // line together.
    int      line_id;

    // Where this line was in the last pass. A label further on still has the
    // value the last pass gave it, but everything before it has grown by at
    // least as much as this line has moved since -- so that much is added to
    // the old value. Without it a short branch or load to a label just ahead
    // looked out of reach after something earlier grew, and was made long for
    // good.
    uint32_t prev_addr;
    uint32_t line_start;   // where this line began in this pass

    // Positions in the order of lines, the same in every pass: line q has its
    // labels at 4q, its body at 4q+1 and a literal pool after its alignment at
    // 4q+2. The alignments of the last pass, in that order, are what a move is
    // rounded by on its way to a label ahead.
    int      seq;
    int      cur_pos;
    struct align { int pos; int pow; } *al, *al_prev;
    int      nal, nal_prev, alcap, alcap_prev;
    int     *pool_pos;
    bool     was_inst;     // the line just done was an instruction
    bool    *frozen;       // per source line: long for good

    // .include: the files, read once each, and every place one is included,
    // each with its own line sizes -- an instruction in an included file
    // must not shrink between passes any more than one in the source may.
    const asm_include_t *inc;
    struct incfile { char path[192]; char *text; char **lines; int *lens; int nlines; } *files;
    int      nfiles;
    struct incinst { int file; uint32_t *sizes; uint32_t *addrs; bool *frozen; } *insts;
    int      ninsts, inst_no;
    int      depth;
    char     inc_path[8][192];
    int      inc_line[8];

    // Local labels 0: to 999:, counted per pass. The k-th definition of n is
    // the symbol "n:k", which no ordinary name can collide with.
    uint16_t local_count[1000];
} as_t;

#define COND_AL 14

static void error(as_t *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void error(as_t *a, const char *fmt, ...)
{
    if (a->line_failed) return;
    a->line_failed = true;
    if (!a->final) return;
    asm_result_t *r = a->r;
    if (r->nerrors < ASM_MAX_ERRORS) {
        asm_error_t *e = &r->errors[r->nerrors];
        e->line = a->line;
        int n = 0;
        // In an included file: say which, and where; the line is the
        // .include in the source.
        if (a->depth) {
            const char *p = a->inc_path[a->depth];
            const char *b = strrchr(p, '/');
            n = snprintf(e->msg, sizeof e->msg, "%s line %d: ", b ? b + 1 : p, a->inc_line[a->depth] + 1);
            if (n < 0 || n >= (int)sizeof e->msg) n = 0;
        }
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->msg + n, sizeof e->msg - (size_t)n, fmt, ap);
        va_end(ap);
    }
    r->nerrors++;
}

// --- output ----------------------------------------------------------------

static void put8(as_t *a, uint8_t b)
{
    if (a->addr >= a->cap) {
        uint32_t n = a->cap ? a->cap * 2 : 4096;
        while (n <= a->addr) n *= 2;
        uint8_t *p = realloc(a->buf, n);
        if (!p) { error(a, "out of memory"); return; }
        memset(p + a->cap, 0, n - a->cap);
        a->buf = p;
        a->cap = n;
    }
    a->buf[a->addr++] = b;
}

static void put16(as_t *a, uint32_t h)
{
    put8(a, (uint8_t)h);
    put8(a, (uint8_t)(h >> 8));
}

static void put32w(as_t *a, uint32_t hw1, uint32_t hw2)
{
    put16(a, hw1);
    put16(a, hw2);
}

static void put_word(as_t *a, val_t v)
{
    if (v.rel == 1) {
        if (a->final) {
            if (a->nrel == a->relcap) {
                uint32_t n = a->relcap ? a->relcap * 2 : 32;
                uint32_t *p = realloc(a->rel, n * sizeof *p);
                if (!p) { error(a, "out of memory"); return; }
                a->rel = p;
                a->relcap = n;
            }
            a->rel[a->nrel++] = a->addr;
        }
        if (v.func) v.v |= 1;
    } else if (v.rel != 0) {
        error(a, "not an address or a number");
    }
    uint32_t w = (uint32_t)v.v;
    put8(a, (uint8_t)w);
    put8(a, (uint8_t)(w >> 8));
    put8(a, (uint8_t)(w >> 16));
    put8(a, (uint8_t)(w >> 24));
}

// --- symbols ---------------------------------------------------------------

static unsigned hash_name(const char *s, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h & 255;
}

static sym_t *sym_find(as_t *a, const char *s, int n, bool create)
{
    if (n > 31) n = 31;
    unsigned h = hash_name(s, n);
    for (int i = a->bucket[h]; i >= 0; i = a->syms[i].next) {
        sym_t *y = &a->syms[i];
        if ((int)strlen(y->name) == n && memcmp(y->name, s, (size_t)n) == 0) return y;
    }
    if (!create) return 0;
    if (a->nsyms == a->symcap) {
        int c = a->symcap ? a->symcap * 2 : 64;
        sym_t *p = realloc(a->syms, (size_t)c * sizeof *p);
        if (!p) return 0;
        a->syms = p;
        a->symcap = c;
    }
    sym_t *y = &a->syms[a->nsyms];
    memset(y, 0, sizeof *y);
    memcpy(y->name, s, (size_t)n);
    y->name[n] = 0;
    y->defpass = -1;
    y->next = a->bucket[h];
    a->bucket[h] = a->nsyms++;
    return y;
}

static void define(as_t *a, const char *s, int n, int32_t v, int rel, bool equ)
{
    sym_t *y = sym_find(a, s, n, true);
    if (!y) { error(a, "out of memory"); return; }
    if (y->defpass == a->pass && y->line != a->line_id && !y->equ) {
        error(a, "%s is defined twice", y->name);
        return;
    }
    if (y->defpass >= 0 && (y->value != v || y->rel != rel)) a->changed = true;
    if (y->defpass < 0 && a->pass > 0) a->changed = true;
    y->value = v;
    y->rel = rel;
    y->equ = equ;
    y->defpass = a->pass;
    y->line = a->line_id;
    y->pos = equ ? a->cur_pos : a->cur_pos - 1;
    if (!equ) {
        y->func = a->next_func;
        a->next_func = false;
    }
}

// --- text ------------------------------------------------------------------

static bool is_id0(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static bool is_id(int c)  { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool ieq(const char *a, int n, const char *b)
{
    if ((int)strlen(b) != n) return false;
    for (int i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != b[i]) return false;
    return true;
}

// Registers by name, or -1. The name must end where the word does.
static int reg_at(const char *p, const char **end)
{
    p = skip_ws(p);
    int n = 0;
    while (is_id(p[n])) n++;
    int r = -1;
    if (n >= 2 && n <= 3 && (p[0] == 'r' || p[0] == 'R') && isdigit((unsigned char)p[1])) {
        int v = p[1] - '0';
        if (n == 3) {
            if (!isdigit((unsigned char)p[2])) return -1;
            v = v * 10 + (p[2] - '0');
        }
        if (v <= 15 && !(n == 3 && p[1] == '0')) r = v;
    } else if (ieq(p, n, "sp")) r = 13;
    else if (ieq(p, n, "lr")) r = 14;
    else if (ieq(p, n, "pc")) r = 15;
    else if (ieq(p, n, "ip")) r = 12;
    else if (ieq(p, n, "fp")) r = 11;
    else if (ieq(p, n, "sl")) r = 10;
    else if (ieq(p, n, "sb")) r = 9;
    if (r >= 0 && end) *end = skip_ws(p + n);
    return r;
}

// A whole operand that is one register.
static int reg_only(const char *s)
{
    const char *e;
    int r = reg_at(s, &e);
    return (r >= 0 && *e == 0) ? r : -1;
}

// --- moving forward --------------------------------------------------------

// How far a label at target_pos should be taken to have moved since the last
// pass: as far as this line has, rounded down at every alignment in between.
static int32_t stretch_to(as_t *a, int target_pos)
{
    int32_t st = (int32_t)(a->line_start - a->prev_addr);
    for (int i = 0; i < a->nal_prev && st; i++) {
        const struct align *g = &a->al_prev[i];
        if (g->pos <= a->cur_pos) continue;
        if (g->pos >= target_pos) break;
        int32_t m = (int32_t)((1u << g->pow) - 1);
        st = st < 0 ? -((-st) & ~m) : (st & ~m);
    }
    return st;
}

static void note_align(as_t *a, int pos, int pow)
{
    if (a->nal == a->alcap) {
        int c = a->alcap ? a->alcap * 2 : 32;
        struct align *p = realloc(a->al, (size_t)c * sizeof *p);
        if (!p) return;
        a->al = p;
        a->alcap = c;
    }
    a->al[a->nal].pos = pos;
    a->al[a->nal].pow = pow;
    a->nal++;
}

// --- expressions -----------------------------------------------------------

typedef struct {
    as_t       *a;
    const char *p;
    bool        bad;
} ex_t;

static val_t ex_or(ex_t *x);

static val_t num(int32_t v) { val_t r = { v, 0, false, false, false }; return r; }

static int char_escape(const char **pp)
{
    const char *p = *pp;
    int c = (unsigned char)*p++;
    if (c != '\\') { *pp = p; return c; }
    c = (unsigned char)*p++;
    switch (c) {
    case 'n': c = '\n'; break;
    case 't': c = '\t'; break;
    case 'r': c = '\r'; break;
    case 'b': c = '\b'; break;
    case 'f': c = '\f'; break;
    case 'e': c = 27; break;
    case 'a': c = 7; break;
    case 'x': case 'X': {
        int v = 0, k = 0;
        while (k < 2 && isxdigit((unsigned char)*p)) {
            int d = *p++;
            v = v * 16 + (isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10));
            k++;
        }
        c = v;
        break;
    }
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0', k = 1;
            while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
            c = v & 0xff;
        }
        break;
    }
    *pp = p;
    return c;
}

static val_t ex_atom(ex_t *x)
{
    x->p = skip_ws(x->p);
    const char *p = x->p;
    if (*p == '(') {
        x->p = p + 1;
        val_t v = ex_or(x);
        x->p = skip_ws(x->p);
        if (*x->p != ')') { x->bad = true; return v; }
        x->p++;
        return v;
    }
    if (*p == '-' || *p == '~' || *p == '!' || *p == '+') {
        x->p = p + 1;
        val_t v = ex_atom(x);
        if (*p == '+') return v;
        if (v.rel) { x->bad = true; return v; }
        if (*p == '-') v.v = -v.v;
        else if (*p == '~') v.v = ~v.v;
        else v.v = !v.v;
        return v;
    }
    if (*p == '\'') {
        p++;
        int c = char_escape(&p);
        if (*p == '\'') p++;
        x->p = p;
        return num(c);
    }
    if (isdigit((unsigned char)*p)) {
        // 1b and 1f: the nearest local label 1 backwards or forwards.
        int d = 0;
        while (isdigit((unsigned char)p[d])) d++;
        if ((p[d] == 'b' || p[d] == 'f' || p[d] == 'B' || p[d] == 'F') && !is_id(p[d + 1]) && d <= 3) {
            int nlab = atoi(p);
            int k = x->a->local_count[nlab] - ((p[d] == 'b' || p[d] == 'B') ? 1 : 0);
            x->p = p + d + 1;
            if (k < 0) { error(x->a, "there is no %d: before this", nlab); return num(0); }
            char nm[16];
            int len = snprintf(nm, sizeof nm, "%d:%d", nlab, k);
            sym_t *y = sym_find(x->a, nm, len, false);
            if (!y || y->defpass < 0) {
                if (x->a->final) error(x->a, "there is no %d: after this", nlab);
                val_t v = { (int32_t)x->a->addr, 1, true, false, false };
                return v;
            }
            val_t v = { y->value, y->rel, false, y->func, false };
            if (y->defpass != x->a->pass) {
                v.v += stretch_to(x->a, y->pos);
                v.est = false;
            }
            return v;
        }
        uint32_t v = 0;
        int base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
        else if (p[0] == '0' && isdigit((unsigned char)p[1])) { base = 8; p += 1; }
        const char *start = p;
        for (;;) {
            int c = tolower((unsigned char)*p);
            int d;
            if (isdigit(c)) d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else break;
            if (d >= base) { x->bad = true; break; }
            v = v * (uint32_t)base + (uint32_t)d;
            p++;
        }
        if (p == start || is_id(*p)) x->bad = true;
        x->p = p;
        return num((int32_t)v);
    }
    if (*p == '.' && !is_id(p[1])) {
        x->p = p + 1;
        val_t v = { (int32_t)x->a->addr, 1, false, false, false };
        return v;
    }
    if (is_id0((unsigned char)*p)) {
        int n = 0;
        while (is_id(p[n])) n++;
        x->p = p + n;
        if (reg_at(p, 0) >= 0) { x->bad = true; return num(0); }
        sym_t *y = sym_find(x->a, p, n, false);
        if (!y || y->defpass < 0) {
            if (x->a->final) error(x->a, "%.*s is not defined", n, p);
            val_t v = { (int32_t)x->a->addr, 1, true, false, false };
            return v;
        }
        val_t v = { y->value, y->rel, false, y->func, false };
        // Defined only in an earlier pass is fine: that is a reference forward,
        // and its value is the one the last pass found, moved on as far as
        // this line has moved.
        if (y->rel && y->defpass != x->a->pass) {
            v.v += stretch_to(x->a, y->pos);
            v.est = false;
        }
        return v;
    }
    x->bad = true;
    return num(0);
}

static val_t combine(ex_t *x, val_t a, val_t b, int op)
{
    val_t r = { 0, 0, a.undef || b.undef, false, a.est || b.est };
    if (op == '+') {
        r.v = a.v + b.v;
        r.rel = a.rel + b.rel;
        r.func = a.rel ? a.func : b.func;
        return r;
    }
    if (op == '-') {
        r.v = a.v - b.v;
        r.rel = a.rel - b.rel;
        r.func = a.func && !b.rel;
        return r;
    }
    if (a.rel || b.rel) { x->bad = true; return r; }
    switch (op) {
    case '*': r.v = a.v * b.v; break;
    case '/': r.v = b.v ? a.v / b.v : 0; if (!b.v && !r.undef) x->bad = true; break;
    case '%': r.v = b.v ? a.v % b.v : 0; if (!b.v && !r.undef) x->bad = true; break;
    case '<': r.v = (int32_t)((uint32_t)a.v << (b.v & 31)); break;
    case '>': r.v = (int32_t)((uint32_t)a.v >> (b.v & 31)); break;
    case '&': r.v = a.v & b.v; break;
    case '|': r.v = a.v | b.v; break;
    case '^': r.v = a.v ^ b.v; break;
    }
    return r;
}

static val_t ex_mul(ex_t *x)
{
    val_t v = ex_atom(x);
    for (;;) {
        x->p = skip_ws(x->p);
        char c = *x->p;
        if (c == '*' || c == '/' || c == '%') { x->p++; v = combine(x, v, ex_atom(x), c); }
        else if ((c == '<' && x->p[1] == '<') || (c == '>' && x->p[1] == '>')) {
            x->p += 2;
            v = combine(x, v, ex_atom(x), c);
        } else return v;
    }
}

static val_t ex_add(ex_t *x)
{
    val_t v = ex_mul(x);
    for (;;) {
        x->p = skip_ws(x->p);
        char c = *x->p;
        if (c == '+' || c == '-') { x->p++; v = combine(x, v, ex_mul(x), c); }
        else return v;
    }
}

static val_t ex_and(ex_t *x)
{
    val_t v = ex_add(x);
    for (;;) {
        x->p = skip_ws(x->p);
        if (*x->p == '&' && x->p[1] != '&') { x->p++; v = combine(x, v, ex_add(x), '&'); }
        else return v;
    }
}

static val_t ex_or(ex_t *x)
{
    val_t v = ex_and(x);
    for (;;) {
        x->p = skip_ws(x->p);
        char c = *x->p;
        if (c == '|' || c == '^') { x->p++; v = combine(x, v, ex_and(x), c); }
        else return v;
    }
}

// The whole string is one expression.
static bool expr(as_t *a, const char *s, val_t *out)
{
    ex_t x = { a, s, false };
    val_t v = ex_or(&x);
    x.p = skip_ws(x.p);
    if (x.bad || *x.p) {
        error(a, "cannot read the expression '%s'", s);
        return false;
    }
    *out = v;
    return true;
}

// An immediate: '#' then a number. The '#' may be left out, as GNU as allows.
static bool imm(as_t *a, const char *s, int32_t *out)
{
    s = skip_ws(s);
    if (*s == '#') s++;
    val_t v;
    if (!expr(a, s, &v)) return false;
    // A name not defined yet is taken as 0 until a later pass knows it; the
    // last pass says if it never was.
    if (v.undef && !a->final) { *out = 0; return true; }
    if (v.rel) { error(a, "an address where a number was wanted"); return false; }
    *out = v.v;
    return true;
}

static bool is_imm(const char *s)
{
    s = skip_ws(s);
    return *s == '#' || ((isdigit((unsigned char)*s) || *s == '-' || *s == '\'' || *s == '(')
                         && reg_only(s) < 0);
}

// --- operands --------------------------------------------------------------

#define MAX_OPS 6

// Split at the commas that are not inside brackets, braces or quotes.
static int split_ops(char *s, char **ops)
{
    int n = 0, depth = 0;
    char q = 0;
    s = (char *)skip_ws(s);
    if (!*s) return 0;
    ops[n++] = s;
    for (char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '"') { q = '"'; continue; }
        if (*p == '\'') {
            // A character constant, not a string: 'x' or '\n'.
            if (p[1] == '\\' && p[2]) p += 2; else if (p[1]) p++;
            if (p[1] == '\'') p++;
            continue;
        }
        if (*p == '[' || *p == '{' || *p == '(') depth++;
        else if (*p == ']' || *p == '}' || *p == ')') depth--;
        else if (*p == ',' && depth == 0) {
            *p = 0;
            if (n == MAX_OPS) return -1;
            ops[n++] = (char *)skip_ws(p + 1);
        }
    }
    for (int i = 0; i < n; i++) {
        char *e = ops[i] + strlen(ops[i]);
        while (e > ops[i] && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    }
    return n;
}

// "lsl #3", "lsr r2", "rrx". type: 0 lsl 1 lsr 2 asr 3 ror; rs >= 0 for a
// register shift. RRX is ror with amount 0.
static bool shift_op(as_t *a, const char *s, int *type, int *amt, int *rs)
{
    s = skip_ws(s);
    int n = 0;
    while (isalpha((unsigned char)s[n])) n++;
    *rs = -1;
    *amt = 0;
    if (ieq(s, n, "rrx")) {
        if (*skip_ws(s + n)) { error(a, "rrx takes no amount"); return false; }
        *type = 3;
        return true;
    }
    if (ieq(s, n, "lsl") || ieq(s, n, "asl")) *type = 0;
    else if (ieq(s, n, "lsr")) *type = 1;
    else if (ieq(s, n, "asr")) *type = 2;
    else if (ieq(s, n, "ror")) *type = 3;
    else { error(a, "'%s' is not a shift", s); return false; }
    const char *rest = skip_ws(s + n);
    int r = reg_only(rest);
    if (r >= 0) { *rs = r; return true; }
    int32_t v;
    if (!imm(a, rest, &v)) return false;
    int lo = (*type == 0) ? 0 : 1, hi = (*type == 0 || *type == 3) ? 31 : 32;
    if (v < lo || v > hi) { error(a, "shift by %d is out of range", (int)v); return false; }
    *amt = v;
    return true;
}

// The shift fields of a 32-bit data-processing instruction.
static uint32_t shift_bits(int type, int amt)
{
    amt &= 31;                       // 32 is written as 0
    return ((uint32_t)(amt >> 2) << 12) | ((uint32_t)(amt & 3) << 6) | ((uint32_t)type << 4);
}

// ThumbExpandImm in reverse, in the order GNU as tries the forms: a plain byte,
// a byte shifted, then the three repeated patterns. -1 when none fits.
static int32_t t32_imm(uint32_t v)
{
    if (v <= 0xff) return (int32_t)v;
    for (int i = 1; i <= 24; i++) {
        if ((v & ~(0xffu << i)) == 0)
            return (int32_t)(((v >> i) & 0x7f) | ((uint32_t)(32 - i) << 7));
    }
    uint32_t b = v & 0xff;
    if (v == ((b << 16) | b)) return (int32_t)(0x100 | b);
    if (v == ((b << 24) | (b << 16) | (b << 8) | b)) return (int32_t)(0x300 | b);
    b = v & 0xff00;
    if (v == ((b << 16) | b)) return (int32_t)(0x200 | (b >> 8));
    return -1;
}

static void put_t32_imm(as_t *a, uint32_t hw1, uint32_t hw2, int32_t imm12)
{
    hw1 |= (uint32_t)((imm12 >> 11) & 1) << 10;
    hw2 |= (uint32_t)((imm12 >> 8) & 7) << 12;
    hw2 |= (uint32_t)(imm12 & 0xff);
    put32w(a, hw1, hw2);
}

// A plain 12-bit immediate (addw, subw, adr), same fields.
static void put_imm12(as_t *a, uint32_t hw1, uint32_t hw2, uint32_t v)
{
    hw1 |= ((v >> 11) & 1) << 10;
    hw2 |= ((v >> 8) & 7) << 12;
    hw2 |= v & 0xff;
    put32w(a, hw1, hw2);
}

// {r0-r3, r7, lr}
static bool reglist(as_t *a, const char *s, uint32_t *mask)
{
    s = skip_ws(s);
    if (*s != '{') { error(a, "a register list in braces was expected"); return false; }
    s++;
    uint32_t m = 0;
    for (;;) {
        s = skip_ws(s);
        if (*s == '}') break;
        const char *e;
        int r1 = reg_at(s, &e);
        if (r1 < 0) { error(a, "not a register in the list"); return false; }
        int r2 = r1;
        if (*e == '-') {
            r2 = reg_at(e + 1, &e);
            if (r2 < r1) { error(a, "a register range must go upwards"); return false; }
        }
        for (int r = r1; r <= r2; r++) m |= 1u << r;
        s = e;
        if (*s == ',') { s++; continue; }
        if (*s == '}') break;
        error(a, "a comma or '}' was expected in the list");
        return false;
    }
    if (*skip_ws(s + 1)) { error(a, "something after the register list"); return false; }
    if (!m) { error(a, "an empty register list"); return false; }
    *mask = m;
    return true;
}

// --- instructions ----------------------------------------------------------

enum {
    OP_MOV, OP_MVN, OP_ADD, OP_SUB, OP_ADC, OP_SBC, OP_RSB, OP_AND, OP_ORR,
    OP_EOR, OP_BIC, OP_ORN, OP_CMP, OP_CMN, OP_TST, OP_TEQ, OP_LSL, OP_LSR,
    OP_ASR, OP_ROR, OP_RRX, OP_MUL, OP_MLA, OP_MLS, OP_SDIV, OP_UDIV,
    OP_UMULL, OP_SMULL, OP_UMLAL, OP_SMLAL, OP_MOVW, OP_MOVT, OP_NEG, OP_ADR,
    OP_ADDW, OP_SUBW,
    OP_LDR, OP_LDRB, OP_LDRH, OP_LDRSB, OP_LDRSH, OP_STR, OP_STRB, OP_STRH,
    OP_LDRD, OP_STRD, OP_LDREX, OP_STREX,
    OP_LDMIA, OP_LDMDB, OP_STMIA, OP_STMDB, OP_PUSH, OP_POP,
    OP_B, OP_BL, OP_BX, OP_BLX, OP_CBZ, OP_CBNZ, OP_TBB, OP_TBH,
    OP_SVC, OP_BKPT, OP_UDF, OP_NOP, OP_WFI, OP_WFE, OP_SEV, OP_YIELD,
    OP_DSB, OP_DMB, OP_ISB, OP_CPSIE, OP_CPSID, OP_MRS, OP_MSR,
    OP_UXTB, OP_UXTH, OP_SXTB, OP_SXTH, OP_REV, OP_REV16, OP_REVSH, OP_RBIT,
    OP_CLZ, OP_BFI, OP_BFC, OP_UBFX, OP_SBFX,
};

#define F_S 1               // takes an 's' for setting flags

typedef struct {
    char    name[7];
    uint8_t op;
    uint8_t flags;
} mnem_t;

// Longest first, so that "ldrb" is tried before "ldr" with a condition "b".
static const mnem_t mnems[] = {
    { "ldrsb", OP_LDRSB, 0 }, { "ldrsh", OP_LDRSH, 0 }, { "ldrex", OP_LDREX, 0 },
    { "strex", OP_STREX, 0 }, { "ldmia", OP_LDMIA, 0 }, { "ldmfd", OP_LDMIA, 0 },
    { "ldmdb", OP_LDMDB, 0 }, { "ldmea", OP_LDMDB, 0 }, { "stmia", OP_STMIA, 0 },
    { "stmea", OP_STMIA, 0 }, { "stmdb", OP_STMDB, 0 }, { "stmfd", OP_STMDB, 0 },
    { "umull", OP_UMULL, 0 }, { "smull", OP_SMULL, 0 }, { "umlal", OP_UMLAL, 0 },
    { "smlal", OP_SMLAL, 0 }, { "yield", OP_YIELD, 0 }, { "cpsie", OP_CPSIE, 0 },
    { "cpsid", OP_CPSID, 0 }, { "rev16", OP_REV16, 0 }, { "revsh", OP_REVSH, 0 },
    { "sdiv", OP_SDIV, 0 }, { "udiv", OP_UDIV, 0 }, { "movw", OP_MOVW, 0 },
    { "movt", OP_MOVT, 0 }, { "addw", OP_ADDW, 0 }, { "subw", OP_SUBW, 0 },
    { "ldrb", OP_LDRB, 0 }, { "ldrh", OP_LDRH, 0 }, { "ldrd", OP_LDRD, 0 },
    { "strb", OP_STRB, 0 }, { "strh", OP_STRH, 0 }, { "strd", OP_STRD, 0 },
    { "push", OP_PUSH, 0 }, { "cbnz", OP_CBNZ, 0 }, { "bkpt", OP_BKPT, 0 },
    { "uxtb", OP_UXTB, 0 }, { "uxth", OP_UXTH, 0 }, { "sxtb", OP_SXTB, 0 },
    { "sxth", OP_SXTH, 0 }, { "rbit", OP_RBIT, 0 }, { "ubfx", OP_UBFX, 0 },
    { "sbfx", OP_SBFX, 0 },
    { "mov", OP_MOV, F_S }, { "mvn", OP_MVN, F_S }, { "add", OP_ADD, F_S },
    { "sub", OP_SUB, F_S }, { "adc", OP_ADC, F_S }, { "sbc", OP_SBC, F_S },
    { "rsb", OP_RSB, F_S }, { "and", OP_AND, F_S }, { "orr", OP_ORR, F_S },
    { "eor", OP_EOR, F_S }, { "bic", OP_BIC, F_S }, { "orn", OP_ORN, F_S },
    { "cmp", OP_CMP, 0 }, { "cmn", OP_CMN, 0 }, { "tst", OP_TST, 0 },
    { "teq", OP_TEQ, 0 }, { "lsl", OP_LSL, F_S }, { "lsr", OP_LSR, F_S },
    { "asr", OP_ASR, F_S }, { "ror", OP_ROR, F_S }, { "rrx", OP_RRX, F_S },
    { "mul", OP_MUL, F_S }, { "mla", OP_MLA, 0 }, { "mls", OP_MLS, 0 },
    { "neg", OP_NEG, F_S }, { "adr", OP_ADR, 0 }, { "ldr", OP_LDR, 0 },
    { "str", OP_STR, 0 }, { "ldm", OP_LDMIA, 0 }, { "stm", OP_STMIA, 0 },
    { "pop", OP_POP, 0 }, { "blx", OP_BLX, 0 }, { "cbz", OP_CBZ, 0 },
    { "tbb", OP_TBB, 0 }, { "tbh", OP_TBH, 0 }, { "svc", OP_SVC, 0 },
    { "udf", OP_UDF, 0 }, { "nop", OP_NOP, 0 }, { "wfi", OP_WFI, 0 },
    { "wfe", OP_WFE, 0 }, { "sev", OP_SEV, 0 }, { "dsb", OP_DSB, 0 },
    { "dmb", OP_DMB, 0 }, { "isb", OP_ISB, 0 }, { "mrs", OP_MRS, 0 },
    { "msr", OP_MSR, 0 }, { "rev", OP_REV, 0 }, { "clz", OP_CLZ, 0 },
    { "bfi", OP_BFI, 0 }, { "bfc", OP_BFC, 0 },
    { "bl", OP_BL, 0 }, { "bx", OP_BX, 0 },
    { "b", OP_B, 0 },
};

static const char conds[] = "eqnecsccmiplvsvchilsgeltgtleal";

static int cond_of(const char *s, int n)
{
    if (n == 0) return COND_AL;
    if (n != 2) return -1;
    char c0 = (char)tolower((unsigned char)s[0]), c1 = (char)tolower((unsigned char)s[1]);
    if (c0 == 'h' && c1 == 's') return 2;
    if (c0 == 'l' && c1 == 'o') return 3;
    for (int i = 0; i < 15; i++)
        if (conds[2 * i] == c0 && conds[2 * i + 1] == c1) return i;
    return -1;
}

typedef struct {
    int  op;
    bool s;
    int  cond;
    char q;              // 'w', 'n' or 0
    bool in_it;
} inst_t;

static bool parse_mnem(const char *m, int n, inst_t *in)
{
    in->q = 0;
    if (n > 2 && m[n - 2] == '.') {
        char c = (char)tolower((unsigned char)m[n - 1]);
        if (c != 'w' && c != 'n') return false;
        in->q = c;
        n -= 2;
    }
    for (size_t k = 0; k < sizeof mnems / sizeof mnems[0]; k++) {
        const mnem_t *e = &mnems[k];
        int len = (int)strlen(e->name);
        if (len > n) continue;
        bool match = true;
        for (int i = 0; i < len; i++)
            if (tolower((unsigned char)m[i]) != e->name[i]) { match = false; break; }
        if (!match) continue;
        const char *rest = m + len;
        int rn = n - len;
        int c;
        if ((e->flags & F_S) && rn >= 1 && tolower((unsigned char)rest[0]) == 's'
            && (c = cond_of(rest + 1, rn - 1)) >= 0) {
            in->op = e->op; in->s = true; in->cond = c;
            return true;
        }
        if ((c = cond_of(rest, rn)) >= 0) {
            in->op = e->op; in->s = false; in->cond = c;
            return true;
        }
    }
    return false;
}

static bool lo(int r) { return r >= 0 && r < 8; }

// May this line be 16 bits? Not when it says .w, and not when an earlier pass
// made it 32: a line never shrinks.
#define NARROW_OK(in) ((in)->q != 'w' && !a->minwide)
// A 16-bit data-processing encoding sets the flags outside an IT block and
// leaves them alone inside one, so whether it can stand for this instruction
// depends on both.
#define FLAGS_OK(in) ((in)->in_it ? !(in)->s : (in)->s)

static bool need_narrow_fail(as_t *a, inst_t *in)
{
    if (in->q == 'n') { error(a, "there is no 16-bit encoding of this"); return true; }
    return false;
}

static bool nops(as_t *a, int n, int want_lo, int want_hi)
{
    if (n < want_lo || n > want_hi) {
        if (want_lo == want_hi) error(a, "%d operands were expected", want_lo);
        else error(a, "%d to %d operands were expected", want_lo, want_hi);
        return false;
    }
    return true;
}

static int need_reg(as_t *a, const char *s)
{
    int r = reg_only(s);
    if (r < 0) error(a, "'%s' is not a register", s);
    return r;
}

// Data-processing op numbers for the 32-bit encodings.
static int dp32(int op)
{
    switch (op) {
    case OP_AND: case OP_TST: return 0;
    case OP_BIC: return 1;
    case OP_ORR: case OP_MOV: return 2;
    case OP_ORN: case OP_MVN: return 3;
    case OP_EOR: case OP_TEQ: return 4;
    case OP_ADD: case OP_CMN: return 8;
    case OP_ADC: return 10;
    case OP_SBC: return 11;
    case OP_SUB: case OP_CMP: return 13;
    case OP_RSB: return 14;
    }
    return 0;
}

// Data-processing op numbers for the 16-bit register encoding.
static int dp16(int op)
{
    switch (op) {
    case OP_AND: return 0;  case OP_EOR: return 1;  case OP_LSL: return 2;
    case OP_LSR: return 3;  case OP_ASR: return 4;  case OP_ADC: return 5;
    case OP_SBC: return 6;  case OP_ROR: return 7;  case OP_TST: return 8;
    case OP_RSB: return 9;  case OP_CMP: return 10; case OP_CMN: return 11;
    case OP_ORR: return 12; case OP_MUL: return 13; case OP_BIC: return 14;
    case OP_MVN: return 15;
    }
    return 0;
}

// 32-bit data processing with a register: Rd, Rn, Rm and an optional shift.
static void dp_reg32(as_t *a, int op, bool s, int rd, int rn, int rm, int type, int amt)
{
    uint32_t hw1 = 0xEA00u | ((uint32_t)dp32(op) << 5) | ((uint32_t)s << 4) | (uint32_t)rn;
    uint32_t hw2 = ((uint32_t)rd << 8) | shift_bits(type, amt) | (uint32_t)rm;
    put32w(a, hw1, hw2);
}

static bool dp_imm32(as_t *a, int op, bool s, int rd, int rn, uint32_t v)
{
    int32_t e = t32_imm(v);
    if (e < 0) return false;
    uint32_t hw1 = 0xF000u | ((uint32_t)dp32(op) << 5) | ((uint32_t)s << 4) | (uint32_t)rn;
    put_t32_imm(a, hw1, (uint32_t)rd << 8, e);
    return true;
}

// The optional third operand of a register form: ", lsl #2".
static bool opt_shift(as_t *a, char **ops, int n, int at, int *type, int *amt, bool *has)
{
    *type = 0; *amt = 0; *has = false;
    if (n <= at) return true;
    int rs;
    if (!shift_op(a, ops[at], type, amt, &rs)) return false;
    if (rs >= 0) { error(a, "a register shift is not allowed here"); return false; }
    *has = true;
    return true;
}

// The value of a label operand, for branches and loads. An unknown one in an
// early pass is taken to be right here, which makes every instruction choose
// its short form first.
static bool target(as_t *a, const char *s, int32_t *t)
{
    val_t v;
    s = skip_ws(s);
    if (*s == '#') s++;
    if (!expr(a, s, &v)) return false;
    if (v.rel != 1) { error(a, "the target must be a label"); return false; }
    *t = v.v;
    return true;
}

static void enc_branch(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 1, 1)) return;
    int32_t t;
    if (!target(a, ops[0], &t)) return;
    int32_t off = t - (int32_t)(a->addr + 4);
    if (off & 1) { error(a, "the target is not on a halfword"); return; }
    bool bl = in->op == OP_BL;
    // The first pass lays out every branch short, as GNU as starts; the passes
    // after it decide.
    if (a->pass == 0 && !bl) off = 0;
    if (!bl) {
        if (in->cond != COND_AL && !in->in_it) {
            if (NARROW_OK(in) && off >= -256 && off <= 254) {
                put16(a, 0xD000u | ((uint32_t)in->cond << 8) | (((uint32_t)off >> 1) & 0xff));
                return;
            }
            if (need_narrow_fail(a, in)) return;
            if (off < -(1 << 20) || off >= (1 << 20)) { error(a, "the branch does not reach"); return; }
            uint32_t S = ((uint32_t)off >> 20) & 1, J2 = ((uint32_t)off >> 19) & 1;
            uint32_t J1 = ((uint32_t)off >> 18) & 1;
            uint32_t hw1 = 0xF000u | (S << 10) | ((uint32_t)in->cond << 6) | (((uint32_t)off >> 12) & 0x3f);
            uint32_t hw2 = 0x8000u | (J1 << 13) | (J2 << 11) | (((uint32_t)off >> 1) & 0x7ff);
            put32w(a, hw1, hw2);
            return;
        }
        if (NARROW_OK(in) && off >= -2048 && off <= 2046) {
            put16(a, 0xE000u | (((uint32_t)off >> 1) & 0x7ff));
            return;
        }
        if (need_narrow_fail(a, in)) return;
    }
    if (off < -(1 << 24) || off >= (1 << 24)) { error(a, "the branch does not reach"); return; }
    uint32_t S = ((uint32_t)off >> 24) & 1, I1 = ((uint32_t)off >> 23) & 1, I2 = ((uint32_t)off >> 22) & 1;
    uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
    uint32_t hw1 = 0xF000u | (S << 10) | (((uint32_t)off >> 12) & 0x3ff);
    uint32_t hw2 = (bl ? 0xD000u : 0x9000u) | (J1 << 13) | (J2 << 11) | (((uint32_t)off >> 1) & 0x7ff);
    put32w(a, hw1, hw2);
}

static void enc_mov(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 2, 3)) return;
    int rd = need_reg(a, ops[0]);
    if (rd < 0) return;
    int rm = reg_only(ops[1]);
    if (rm >= 0) {
        int type, amt; bool has;
        if (n == 3) {
            // mov rd, rm, lsl #n is the shift instruction under another name;
            // mov rd, rm, lsl rs likewise.
            int rs;
            if (!shift_op(a, ops[2], &type, &amt, &rs)) return;
            if (in->op == OP_MOV) {
                if (rs >= 0) {
                    if (NARROW_OK(in) && rd == rm && lo(rd) && lo(rs) && FLAGS_OK(in) && type != 3) {
                        static const uint8_t d[] = { 2, 3, 4, 7 };
                        put16(a, 0x4000u | ((uint32_t)d[type] << 6) | ((uint32_t)rs << 3) | (uint32_t)rd);
                        return;
                    }
                    put32w(a, 0xFA00u | ((uint32_t)type << 5) | ((uint32_t)in->s << 4) | (uint32_t)rm,
                           0xF000u | ((uint32_t)rd << 8) | (uint32_t)rs);
                    return;
                }
                if (NARROW_OK(in) && lo(rd) && lo(rm) && FLAGS_OK(in) && type != 3) {
                    put16(a, ((uint32_t)type << 11) | ((uint32_t)(amt & 31) << 6) | ((uint32_t)rm << 3) | (uint32_t)rd);
                    return;
                }
                dp_reg32(a, OP_MOV, in->s, rd, 15, rm, type, amt);
                return;
            }
            if (rs >= 0) { error(a, "mvn cannot shift by a register"); return; }
            dp_reg32(a, OP_MVN, in->s, rd, 15, rm, type, amt);
            return;
        }
        (void)has;
        if (in->op == OP_MOV) {
            if (in->s) {
                if (NARROW_OK(in) && lo(rd) && lo(rm) && !in->in_it) {
                    put16(a, ((uint32_t)rm << 3) | (uint32_t)rd);
                    return;
                }
            } else if (NARROW_OK(in)) {
                put16(a, 0x4600u | ((uint32_t)(rd & 8) << 4) | ((uint32_t)rm << 3) | (uint32_t)(rd & 7));
                return;
            }
            if (need_narrow_fail(a, in)) return;
            dp_reg32(a, OP_MOV, in->s, rd, 15, rm, 0, 0);
            return;
        }
        if (NARROW_OK(in) && lo(rd) && lo(rm) && FLAGS_OK(in)) {
            put16(a, 0x43C0u | ((uint32_t)rm << 3) | (uint32_t)rd);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        dp_reg32(a, OP_MVN, in->s, rd, 15, rm, 0, 0);
        return;
    }
    if (n != 2) { error(a, "too many operands"); return; }
    int32_t v;
    if (!imm(a, ops[1], &v)) return;
    if (in->op == OP_MOV) {
        if (NARROW_OK(in) && lo(rd) && v >= 0 && v <= 255 && FLAGS_OK(in)) {
            put16(a, 0x2000u | ((uint32_t)rd << 8) | (uint32_t)v);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        if (dp_imm32(a, OP_MOV, in->s, rd, 15, (uint32_t)v)) return;
        if (dp_imm32(a, OP_MVN, in->s, rd, 15, ~(uint32_t)v)) return;
        if (!in->s && v >= 0 && v <= 0xffff) {
            uint32_t u = (uint32_t)v;
            put32w(a, 0xF240u | (((u >> 11) & 1) << 10) | (u >> 12),
                   (((u >> 8) & 7) << 12) | ((uint32_t)rd << 8) | (u & 0xff));
            return;
        }
        error(a, "the constant 0x%x needs ldr rX, =0x%x", (unsigned)v, (unsigned)v);
        return;
    }
    if (need_narrow_fail(a, in)) return;
    if (dp_imm32(a, OP_MVN, in->s, rd, 15, (uint32_t)v)) return;
    if (dp_imm32(a, OP_MOV, in->s, rd, 15, ~(uint32_t)v)) return;
    error(a, "the constant 0x%x cannot be encoded; try ldr rX, =0x%x", (unsigned)v, (unsigned)v);
}

static void enc_movwt(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 2, 2)) return;
    int rd = need_reg(a, ops[0]);
    int32_t v;
    if (rd < 0 || !imm(a, ops[1], &v)) return;
    if (v < 0 || v > 0xffff) { error(a, "%d does not fit in 16 bits", (int)v); return; }
    uint32_t u = (uint32_t)v;
    put32w(a, (in->op == OP_MOVW ? 0xF240u : 0xF2C0u) | (((u >> 11) & 1) << 10) | (u >> 12),
           (((u >> 8) & 7) << 12) | ((uint32_t)rd << 8) | (u & 0xff));
}

static void enc_addsub(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 2, 4)) return;
    int op = in->op;
    int rd = need_reg(a, ops[0]);
    if (rd < 0) return;
    int rn = rd;
    int at = 1;
    if (n >= 3 || (n == 2 && 0)) {
        int r = reg_only(ops[1]);
        if (r >= 0 && n >= 3) { rn = r; at = 2; }
    }
    if (n == 4 && at != 2) { error(a, "too many operands"); return; }
    const char *src = ops[at];
    int rm = reg_only(src);
    bool is_add = op == OP_ADD || op == OP_ADDW;
    if (rm >= 0) {
        if (op == OP_ADDW || op == OP_SUBW) { error(a, "addw and subw take an immediate"); return; }
        int type, amt; bool has;
        if (!opt_shift(a, ops, n, at + 1, &type, &amt, &has)) return;
        if (!has && NARROW_OK(in)) {
            if (lo(rd) && lo(rn) && lo(rm) && FLAGS_OK(in)) {
                put16(a, (is_add ? 0x1800u : 0x1A00u) | ((uint32_t)rm << 6) | ((uint32_t)rn << 3) | (uint32_t)rd);
                return;
            }
            if (is_add && !in->s && (rd == rn || rd == rm)) {
                int other = (rd == rn) ? rm : rn;
                put16(a, 0x4400u | ((uint32_t)(rd & 8) << 4) | ((uint32_t)other << 3) | (uint32_t)(rd & 7));
                return;
            }
        }
        if (need_narrow_fail(a, in)) return;
        dp_reg32(a, is_add ? OP_ADD : OP_SUB, in->s, rd, rn, rm, type, amt);
        return;
    }
    if (n > at + 1) { error(a, "too many operands"); return; }
    int32_t v;
    if (!imm(a, src, &v)) return;
    if (op == OP_ADDW || op == OP_SUBW) {
        if (v < 0 || v > 4095) { error(a, "%d does not fit in 12 bits", (int)v); return; }
        put_imm12(a, op == OP_ADDW ? 0xF200u | (uint32_t)rn : 0xF2A0u | (uint32_t)rn,
                  (uint32_t)rd << 8, (uint32_t)v);
        return;
    }
    uint32_t u = (uint32_t)v;
    // GNU as turns a negative constant into the opposite instruction only in
    // the 32-bit encodings; a 16-bit one is never chosen for it.
    if (NARROW_OK(in) && v >= 0) {
        if (rd == 13 && rn == 13 && !in->s && (u & 3) == 0 && u <= 508) {
            put16(a, (is_add ? 0xB000u : 0xB080u) | (u >> 2));
            return;
        }
        if (is_add && rn == 13 && lo(rd) && !in->s && (u & 3) == 0 && u <= 1020) {
            put16(a, 0xA800u | ((uint32_t)rd << 8) | (u >> 2));
            return;
        }
        if (is_add && rn == 15 && lo(rd) && !in->s && (u & 3) == 0 && u <= 1020) {
            put16(a, 0xA000u | ((uint32_t)rd << 8) | (u >> 2));
            return;
        }
        if (lo(rd) && lo(rn) && FLAGS_OK(in)) {
            if (rd == rn && u <= 255) {
                put16(a, (is_add ? 0x3000u : 0x3800u) | ((uint32_t)rd << 8) | u);
                return;
            }
            if (u <= 7) {
                put16(a, (is_add ? 0x1C00u : 0x1E00u) | (u << 6) | ((uint32_t)rn << 3) | (uint32_t)rd);
                return;
            }
        }
    }
    if (need_narrow_fail(a, in)) return;
    if (v < 0 && v != INT32_MIN) { v = -v; u = (uint32_t)v; is_add = !is_add; }
    if (dp_imm32(a, is_add ? OP_ADD : OP_SUB, in->s, rd, rn, u)) return;
    if (!in->s && u <= 4095) {
        put_imm12(a, (is_add ? 0xF200u : 0xF2A0u) | (uint32_t)rn, (uint32_t)rd << 8, u);
        return;
    }
    error(a, "the constant 0x%x cannot be encoded; try ldr rX, =0x%x", (unsigned)v, (unsigned)v);
}

static void enc_logic(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 2, 4)) return;
    int op = in->op;
    int rd = need_reg(a, ops[0]);
    if (rd < 0) return;
    int rn = rd, at = 1;
    if (n >= 3 && reg_only(ops[1]) >= 0 && (reg_only(ops[2]) >= 0 || is_imm(ops[2]))) {
        rn = reg_only(ops[1]);
        at = 2;
    }
    int rm = reg_only(ops[at]);
    if (rm >= 0) {
        int type, amt; bool has;
        if (!opt_shift(a, ops, n, at + 1, &type, &amt, &has)) return;
        if (!has && NARROW_OK(in) && op != OP_ORN && lo(rd) && lo(rn) && lo(rm) && FLAGS_OK(in)) {
            bool comm = op == OP_AND || op == OP_ORR || op == OP_EOR || op == OP_ADC;
            int other = -1;
            if (rd == rn) other = rm;
            else if (comm && rd == rm) other = rn;
            if (other >= 0) {
                put16(a, 0x4000u | ((uint32_t)dp16(op) << 6) | ((uint32_t)other << 3) | (uint32_t)rd);
                return;
            }
        }
        if (need_narrow_fail(a, in)) return;
        dp_reg32(a, op, in->s, rd, rn, rm, type, amt);
        return;
    }
    if (n > at + 1) { error(a, "too many operands"); return; }
    int32_t v;
    if (!imm(a, ops[at], &v)) return;
    if (need_narrow_fail(a, in)) return;
    if (dp_imm32(a, op, in->s, rd, rn, (uint32_t)v)) return;
    int alt = -1;
    switch (op) {
    case OP_AND: alt = OP_BIC; break; case OP_BIC: alt = OP_AND; break;
    case OP_ORR: alt = OP_ORN; break; case OP_ORN: alt = OP_ORR; break;
    case OP_ADC: alt = OP_SBC; break; case OP_SBC: alt = OP_ADC; break;
    }
    if (alt >= 0 && dp_imm32(a, alt, in->s, rd, rn, ~(uint32_t)v)) return;
    error(a, "the constant 0x%x cannot be encoded; try ldr rX, =0x%x", (unsigned)v, (unsigned)v);
}

static void enc_rsb(as_t *a, inst_t *in, char **ops, int n)
{
    int rd, rn, at;
    if (in->op == OP_NEG) {
        if (!nops(a, n, 2, 2)) return;
        rd = need_reg(a, ops[0]);
        rn = need_reg(a, ops[1]);
        if (rd < 0 || rn < 0) return;
        if (NARROW_OK(in) && lo(rd) && lo(rn) && FLAGS_OK(in)) {
            put16(a, 0x4240u | ((uint32_t)rn << 3) | (uint32_t)rd);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        dp_imm32(a, OP_RSB, in->s, rd, rn, 0);
        return;
    }
    if (!nops(a, n, 2, 4)) return;
    rd = need_reg(a, ops[0]);
    if (rd < 0) return;
    rn = rd; at = 1;
    if (n >= 3) { rn = need_reg(a, ops[1]); at = 2; if (rn < 0) return; }
    int rm = reg_only(ops[at]);
    if (rm >= 0) {
        int type, amt; bool has;
        if (!opt_shift(a, ops, n, at + 1, &type, &amt, &has)) return;
        if (need_narrow_fail(a, in)) return;
        dp_reg32(a, OP_RSB, in->s, rd, rn, rm, type, amt);
        return;
    }
    int32_t v;
    if (!imm(a, ops[at], &v)) return;
    if (v == 0 && NARROW_OK(in) && lo(rd) && lo(rn) && FLAGS_OK(in)) {
        put16(a, 0x4240u | ((uint32_t)rn << 3) | (uint32_t)rd);
        return;
    }
    if (need_narrow_fail(a, in)) return;
    if (!dp_imm32(a, OP_RSB, in->s, rd, rn, (uint32_t)v))
        error(a, "the constant 0x%x cannot be encoded; try ldr rX, =0x%x", (unsigned)v, (unsigned)v);
}

static void enc_compare(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 2, 3)) return;
    int op = in->op;
    int rn = need_reg(a, ops[0]);
    if (rn < 0) return;
    int rm = reg_only(ops[1]);
    if (rm >= 0) {
        int type, amt; bool has;
        if (!opt_shift(a, ops, n, 2, &type, &amt, &has)) return;
        if (!has && NARROW_OK(in)) {
            if (lo(rn) && lo(rm) && op != OP_TEQ) {
                put16(a, 0x4000u | ((uint32_t)dp16(op) << 6) | ((uint32_t)rm << 3) | (uint32_t)rn);
                return;
            }
            if (op == OP_CMP) {
                put16(a, 0x4500u | ((uint32_t)(rn & 8) << 4) | ((uint32_t)rm << 3) | (uint32_t)(rn & 7));
                return;
            }
        }
        if (need_narrow_fail(a, in)) return;
        dp_reg32(a, op, true, 15, rn, rm, type, amt);
        return;
    }
    if (n != 2) { error(a, "too many operands"); return; }
    int32_t v;
    if (!imm(a, ops[1], &v)) return;
    if (op == OP_CMP && NARROW_OK(in) && lo(rn) && v >= 0 && v <= 255) {
        put16(a, 0x2800u | ((uint32_t)rn << 8) | (uint32_t)v);
        return;
    }
    if (need_narrow_fail(a, in)) return;
    if (dp_imm32(a, op, true, 15, rn, (uint32_t)v)) return;
    if (op == OP_CMP && dp_imm32(a, OP_CMN, true, 15, rn, (uint32_t)-v)) return;
    if (op == OP_CMN && dp_imm32(a, OP_CMP, true, 15, rn, (uint32_t)-v)) return;
    error(a, "the constant 0x%x cannot be encoded; try ldr rX, =0x%x", (unsigned)v, (unsigned)v);
}

static void enc_shift(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    if (op == OP_RRX) {
        if (!nops(a, n, 2, 2)) return;
        int rd = need_reg(a, ops[0]), rm = need_reg(a, ops[1]);
        if (rd < 0 || rm < 0) return;
        dp_reg32(a, OP_MOV, in->s, rd, 15, rm, 3, 0);
        return;
    }
    if (!nops(a, n, 2, 3)) return;
    int type = op == OP_LSL ? 0 : op == OP_LSR ? 1 : op == OP_ASR ? 2 : 3;
    int rd = need_reg(a, ops[0]);
    if (rd < 0) return;
    int rm = rd;
    int at = 1;
    if (n == 3) { rm = need_reg(a, ops[1]); at = 2; if (rm < 0) return; }
    int rs = reg_only(ops[at]);
    if (rs >= 0) {
        if (NARROW_OK(in) && rd == rm && lo(rd) && lo(rs) && FLAGS_OK(in)) {
            put16(a, 0x4000u | ((uint32_t)dp16(op) << 6) | ((uint32_t)rs << 3) | (uint32_t)rd);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        put32w(a, 0xFA00u | ((uint32_t)type << 5) | ((uint32_t)in->s << 4) | (uint32_t)rm,
               0xF000u | ((uint32_t)rd << 8) | (uint32_t)rs);
        return;
    }
    int32_t v;
    if (!imm(a, ops[at], &v)) return;
    int lo_ok = type == 0 ? 0 : 1, hi = (type == 0 || type == 3) ? 31 : 32;
    if (v < lo_ok || v > hi) { error(a, "shift by %d is out of range", (int)v); return; }
    if (type != 3 && NARROW_OK(in) && lo(rd) && lo(rm) && FLAGS_OK(in)) {
        put16(a, ((uint32_t)type << 11) | ((uint32_t)(v & 31) << 6) | ((uint32_t)rm << 3) | (uint32_t)rd);
        return;
    }
    if (need_narrow_fail(a, in)) return;
    dp_reg32(a, OP_MOV, in->s, rd, 15, rm, type, v);
}

static void enc_mul(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    if (op == OP_MUL) {
        if (!nops(a, n, 2, 3)) return;
        int rd = need_reg(a, ops[0]);
        int rn = need_reg(a, ops[1]);
        int rm = n == 3 ? need_reg(a, ops[2]) : rd;
        if (rd < 0 || rn < 0 || rm < 0) return;
        if (NARROW_OK(in) && lo(rd) && lo(rn) && lo(rm) && FLAGS_OK(in) && (rd == rm || rd == rn)) {
            int other = rd == rm ? rn : rm;
            put16(a, 0x4340u | ((uint32_t)other << 3) | (uint32_t)rd);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        if (in->s) { error(a, "muls has only a 16-bit form, for low registers"); return; }
        put32w(a, 0xFB00u | (uint32_t)rn, 0xF000u | ((uint32_t)rd << 8) | (uint32_t)rm);
        return;
    }
    if (op == OP_MLA || op == OP_MLS) {
        if (!nops(a, n, 4, 4)) return;
        int rd = need_reg(a, ops[0]), rn = need_reg(a, ops[1]);
        int rm = need_reg(a, ops[2]), ra = need_reg(a, ops[3]);
        if (rd < 0 || rn < 0 || rm < 0 || ra < 0) return;
        put32w(a, 0xFB00u | (uint32_t)rn,
               ((uint32_t)ra << 12) | ((uint32_t)rd << 8) | (op == OP_MLS ? 0x10u : 0) | (uint32_t)rm);
        return;
    }
    if (op == OP_SDIV || op == OP_UDIV) {
        if (!nops(a, n, 2, 3)) return;
        int rd = need_reg(a, ops[0]);
        int rn = n == 3 ? need_reg(a, ops[1]) : rd;
        int rm = need_reg(a, ops[n - 1]);
        if (rd < 0 || rn < 0 || rm < 0) return;
        put32w(a, (op == OP_SDIV ? 0xFB90u : 0xFBB0u) | (uint32_t)rn,
               0xF0F0u | ((uint32_t)rd << 8) | (uint32_t)rm);
        return;
    }
    if (!nops(a, n, 4, 4)) return;
    int lo_r = need_reg(a, ops[0]), hi_r = need_reg(a, ops[1]);
    int rn = need_reg(a, ops[2]), rm = need_reg(a, ops[3]);
    if (lo_r < 0 || hi_r < 0 || rn < 0 || rm < 0) return;
    uint32_t base = op == OP_UMULL ? 0xFBA0u : op == OP_SMULL ? 0xFB80u
                  : op == OP_UMLAL ? 0xFBE0u : 0xFBC0u;
    put32w(a, base | (uint32_t)rn, ((uint32_t)lo_r << 12) | ((uint32_t)hi_r << 8) | (uint32_t)rm);
}

static void enc_extend(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    if (op == OP_CLZ || op == OP_RBIT || op == OP_REV || op == OP_REV16 || op == OP_REVSH) {
        if (!nops(a, n, 2, 2)) return;
        int rd = need_reg(a, ops[0]), rm = need_reg(a, ops[1]);
        if (rd < 0 || rm < 0) return;
        if (NARROW_OK(in) && lo(rd) && lo(rm) && (op == OP_REV || op == OP_REV16 || op == OP_REVSH)) {
            uint32_t b = op == OP_REV ? 0xBA00u : op == OP_REV16 ? 0xBA40u : 0xBAC0u;
            put16(a, b | ((uint32_t)rm << 3) | (uint32_t)rd);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        uint32_t hw1 = (op == OP_CLZ ? 0xFAB0u : 0xFA90u) | (uint32_t)rm;
        uint32_t lo2 = op == OP_CLZ ? 0x80 : op == OP_REV ? 0x80 : op == OP_REV16 ? 0x90
                     : op == OP_RBIT ? 0xA0 : 0xB0;
        put32w(a, hw1, 0xF000u | lo2 | ((uint32_t)rd << 8) | (uint32_t)rm);
        return;
    }
    if (!nops(a, n, 2, 3)) return;
    int rd = need_reg(a, ops[0]), rm = need_reg(a, ops[1]);
    if (rd < 0 || rm < 0) return;
    int rot = 0;
    if (n == 3) {
        int type, amt, rs;
        if (!shift_op(a, ops[2], &type, &amt, &rs)) return;
        if (type != 3 || rs >= 0 || (amt != 8 && amt != 16 && amt != 24)) {
            error(a, "the rotation must be ror #8, #16 or #24");
            return;
        }
        rot = amt / 8;
    }
    if (NARROW_OK(in) && !rot && lo(rd) && lo(rm)) {
        uint32_t b = op == OP_UXTB ? 0xB2C0u : op == OP_UXTH ? 0xB280u : op == OP_SXTB ? 0xB240u : 0xB200u;
        put16(a, b | ((uint32_t)rm << 3) | (uint32_t)rd);
        return;
    }
    if (need_narrow_fail(a, in)) return;
    uint32_t hw1 = op == OP_UXTB ? 0xFA5Fu : op == OP_UXTH ? 0xFA1Fu : op == OP_SXTB ? 0xFA4Fu : 0xFA0Fu;
    put32w(a, hw1, 0xF080u | ((uint32_t)rd << 8) | ((uint32_t)rot << 4) | (uint32_t)rm);
}

static void enc_bitfield(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    int want = op == OP_BFC ? 3 : 4;
    if (!nops(a, n, want, want)) return;
    int rd = need_reg(a, ops[0]);
    int rn = op == OP_BFC ? 15 : need_reg(a, ops[1]);
    int32_t lsb, width;
    if (rd < 0 || rn < 0 || !imm(a, ops[want - 2], &lsb) || !imm(a, ops[want - 1], &width)) return;
    if (lsb < 0 || lsb > 31 || width < 1 || lsb + width > 32) { error(a, "the bit field does not fit"); return; }
    uint32_t base = op == OP_BFI || op == OP_BFC ? 0xF360u : op == OP_UBFX ? 0xF3C0u : 0xF340u;
    uint32_t last = (op == OP_BFI || op == OP_BFC) ? (uint32_t)(lsb + width - 1) : (uint32_t)(width - 1);
    put32w(a, base | (uint32_t)rn,
           (((uint32_t)lsb >> 2) << 12) | ((uint32_t)rd << 8) | (((uint32_t)lsb & 3) << 6) | last);
}

// --- loads and stores ------------------------------------------------------

typedef struct {
    int rn;
    int rm;             // -1 for an immediate offset
    int shift;          // lsl amount with a register
    int32_t off;
    bool neg;           // the offset was written with a minus
    bool pre, wb;       // pre-indexed, write back
} addr_t;

// "[rn]", "[rn, #off]", "[rn, #off]!", "[rn, rm{, lsl #n}]", and the post-index
// form "[rn], #off" whose offset arrives as the next operand.
static bool parse_addr(as_t *a, const char *s, const char *post, addr_t *m)
{
    memset(m, 0, sizeof *m);
    m->rm = -1;
    m->pre = true;
    s = skip_ws(s);
    if (*s != '[') { error(a, "an address in brackets was expected"); return false; }
    char tmp[LINE_MAX_LEN];
    size_t len = strlen(s);
    if (len >= sizeof tmp) { error(a, "the operand is too long"); return false; }
    memcpy(tmp, s + 1, len);
    char *close = strrchr(tmp, ']');
    if (!close) { error(a, "the ']' is missing"); return false; }
    const char *after = skip_ws(close + 1);
    if (*after == '!') { m->wb = true; after = skip_ws(after + 1); }
    if (*after) { error(a, "something after the address"); return false; }
    *close = 0;
    char *parts[MAX_OPS];
    int np = split_ops(tmp, parts);
    if (np < 1 || np > 3) { error(a, "the address cannot be read"); return false; }
    m->rn = need_reg(a, parts[0]);
    if (m->rn < 0) return false;
    if (np >= 2) {
        const char *p = skip_ws(parts[1]);
        bool minus = false;
        if (*p == '-' && reg_only(p + 1) >= 0) { minus = true; p++; }
        int rm = reg_only(p);
        if (rm >= 0) {
            if (minus) { error(a, "a subtracted register offset is not in Thumb"); return false; }
            m->rm = rm;
            if (np == 3) {
                int type, amt, rs;
                if (!shift_op(a, parts[2], &type, &amt, &rs)) return false;
                if (type != 0 || rs >= 0 || amt > 3) { error(a, "only lsl #0 to #3 is allowed here"); return false; }
                m->shift = amt;
            }
        } else {
            if (np == 3) { error(a, "the address cannot be read"); return false; }
            if (!imm(a, parts[1], &m->off)) return false;
            const char *q = skip_ws(parts[1]);
            if (*q == '#') q = skip_ws(q + 1);
            m->neg = *q == '-';
        }
    }
    if (post) {
        if (np != 1 || m->wb) { error(a, "a post-indexed address is [rn], #offset"); return false; }
        if (!imm(a, post, &m->off)) return false;
        m->pre = false;
        m->wb = true;
        const char *q = skip_ws(post);
        if (*q == '#') q = skip_ws(q + 1);
        m->neg = *q == '-';
    }
    return true;
}

static int lsize(int op)
{
    switch (op) {
    case OP_LDRB: case OP_STRB: case OP_LDRSB: return 1;
    case OP_LDRH: case OP_STRH: case OP_LDRSH: return 2;
    }
    return 4;
}

// The 32-bit T4 base of each load and store; T3 is the same with bit 7 set.
static uint32_t ls_t4(int op)
{
    switch (op) {
    case OP_STR: return 0xF840u;   case OP_STRB: return 0xF800u;
    case OP_STRH: return 0xF820u;  case OP_LDR: return 0xF850u;
    case OP_LDRB: return 0xF810u;  case OP_LDRH: return 0xF830u;
    case OP_LDRSB: return 0xF910u; case OP_LDRSH: return 0xF930u;
    }
    return 0;
}

// est: the target is only an estimate from the last pass, so only its
// distance is believed -- not its alignment, not that it is ahead. The last
// pass, where nothing is estimated, settles those.
static void enc_literal_load(as_t *a, inst_t *in, int rt, int32_t t, bool known, bool est)
{
    uint32_t pc = (a->addr + 4) & ~3u;
    int32_t off = t - (int32_t)pc;
    if (a->pass == 0) known = false;           // short in the first layout
    if (in->op == OP_LDR && NARROW_OK(in) && lo(rt)
        && (!known || (est ? off <= 1020 : ((t & 3) == 0 && off >= 0 && off <= 1020)))) {
        put16(a, 0x4800u | ((uint32_t)rt << 8) | (known && off >= 0 ? ((uint32_t)off >> 2) & 0xff : 0));
        return;
    }
    if (need_narrow_fail(a, in)) return;
    if (known && (off < -4095 || off > 4095)) {
        if (est) off = 0; else { error(a, "the literal is out of reach"); return; }
    }
    uint32_t u = off < 0 ? (uint32_t)-off : (uint32_t)off;
    put32w(a, ls_t4(in->op) | 0xFu | (off >= 0 ? 0x80u : 0), ((uint32_t)rt << 12) | (known ? u : 0));
}

// ldr rX, =value: a 32-bit move when the value fits one, as GNU as does, and
// otherwise a word in the next literal pool.
static void enc_ldr_const(as_t *a, inst_t *in, int rt, const char *s)
{
    val_t v;
    if (!expr(a, s, &v)) return;
    if (v.rel == 0 && !v.undef && in->q != 'w') {
        uint32_t u = (uint32_t)v.v;
        // Never the 16-bit movs: it would change the flags, which a load
        // does not. GNU as uses mov.w here for the same reason.
        {
            if (dp_imm32(a, OP_MOV, false, rt, 15, u)) return;
            if (dp_imm32(a, OP_MVN, false, rt, 15, ~u)) return;
            if (u <= 0xffff) {
                put32w(a, 0xF240u | (((u >> 11) & 1) << 10) | (u >> 12),
                       (((u >> 8) & 7) << 12) | ((uint32_t)rt << 8) | (u & 0xff));
                return;
            }
        }
    } else if (v.rel != 0 && v.rel != 1) {
        error(a, "not an address or a number");
        return;
    }
    // Into the pool, once per value.
    int k;
    for (k = a->pool_first; k < a->nlit; k++)
        if (a->lit[k].v == v.v && a->lit[k].rel == v.rel && a->lit[k].func == v.func) break;
    if (k == a->nlit) {
        if (a->nlit == a->litcap) {
            int c = a->litcap ? a->litcap * 2 : 32;
            lit_t *p = realloc(a->lit, (size_t)c * sizeof *p);
            if (!p) { error(a, "out of memory"); return; }
            a->lit = p;
            a->litcap = c;
        }
        a->lit[a->nlit].v = v.v;
        a->lit[a->nlit].rel = v.rel;
        a->lit[a->nlit].func = v.func;
        a->nlit++;
    }
    int idx = k - a->pool_first;
    bool known = a->pool_no < a->npool_cap && a->pool_known[a->pool_no];
    int32_t t = known ? (int32_t)a->pool_addr[a->pool_no] + 4 * idx
                        + stretch_to(a, a->pool_pos[a->pool_no] + 1)
                      : (int32_t)a->addr;
    // Exact only in the last pass and only when this line has not moved: a
    // line that moved makes the pass change something, so another follows.
    enc_literal_load(a, in, rt, t, known, false);
}

static void enc_ldst(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    bool load = op == OP_LDR || op == OP_LDRB || op == OP_LDRH || op == OP_LDRSB || op == OP_LDRSH;
    if (!nops(a, n, 2, 3)) return;
    int rt = need_reg(a, ops[0]);
    if (rt < 0) return;
    const char *s = skip_ws(ops[1]);
    if (*s == '=') {
        if (op != OP_LDR || n != 2) { error(a, "only ldr takes =value"); return; }
        enc_ldr_const(a, in, rt, s + 1);
        return;
    }
    if (*s != '[') {
        if (!load || n != 2) { error(a, "an address in brackets was expected"); return; }
        val_t v;
        if (!expr(a, s, &v)) return;
        if (v.rel != 1) { error(a, "the target must be a label"); return; }
        enc_literal_load(a, in, rt, v.v, !v.undef, v.est);
        return;
    }
    addr_t m;
    if (!parse_addr(a, ops[1], n == 3 ? ops[2] : 0, &m)) return;
    int sz = lsize(op);
    uint32_t t4 = ls_t4(op);
    if (m.rm >= 0) {
        if (m.wb) { error(a, "a register offset cannot write back"); return; }
        if (NARROW_OK(in) && !m.shift && lo(rt) && lo(m.rn) && lo(m.rm)) {
            uint32_t b = 0;
            switch (op) {
            case OP_STR: b = 0x5000; break;   case OP_STRH: b = 0x5200; break;
            case OP_STRB: b = 0x5400; break;  case OP_LDRSB: b = 0x5600; break;
            case OP_LDR: b = 0x5800; break;   case OP_LDRH: b = 0x5A00; break;
            case OP_LDRB: b = 0x5C00; break;  case OP_LDRSH: b = 0x5E00; break;
            }
            put16(a, b | ((uint32_t)m.rm << 6) | ((uint32_t)m.rn << 3) | (uint32_t)rt);
            return;
        }
        if (need_narrow_fail(a, in)) return;
        put32w(a, t4 | (uint32_t)m.rn, ((uint32_t)rt << 12) | ((uint32_t)m.shift << 4) | (uint32_t)m.rm);
        return;
    }
    int32_t off = m.off;
    if (m.pre && !m.wb && !m.neg && off >= 0 && NARROW_OK(in)) {
        if (lo(rt) && lo(m.rn) && (off % sz) == 0 && off <= 31 * sz && op != OP_LDRSB && op != OP_LDRSH) {
            uint32_t b = 0;
            switch (op) {
            case OP_STR: b = 0x6000; break;  case OP_LDR: b = 0x6800; break;
            case OP_STRB: b = 0x7000; break; case OP_LDRB: b = 0x7800; break;
            case OP_STRH: b = 0x8000; break; case OP_LDRH: b = 0x8800; break;
            }
            put16(a, b | ((uint32_t)(off / sz) << 6) | ((uint32_t)m.rn << 3) | (uint32_t)rt);
            return;
        }
        if (sz == 4 && m.rn == 13 && lo(rt) && (off & 3) == 0 && off <= 1020) {
            put16(a, (load ? 0x9800u : 0x9000u) | ((uint32_t)rt << 8) | ((uint32_t)off >> 2));
            return;
        }
    }
    if (need_narrow_fail(a, in)) return;
    if (m.pre && !m.wb && !m.neg && off >= 0 && off <= 4095) {
        put32w(a, (t4 | 0x80u) | (uint32_t)m.rn, ((uint32_t)rt << 12) | (uint32_t)off);
        return;
    }
    uint32_t u = off < 0 ? (uint32_t)-off : (uint32_t)off;
    if (u > 255) { error(a, "the offset %d is out of range", (int)off); return; }
    bool up = off > 0 || (off == 0 && !m.neg);
    put32w(a, t4 | (uint32_t)m.rn,
           ((uint32_t)rt << 12) | 0x800u | ((uint32_t)m.pre << 10) | ((uint32_t)up << 9)
           | ((uint32_t)m.wb << 8) | u);
}

static void enc_ldst_d(as_t *a, inst_t *in, char **ops, int n)
{
    if (!nops(a, n, 3, 4)) return;
    int rt = need_reg(a, ops[0]), rt2 = need_reg(a, ops[1]);
    if (rt < 0 || rt2 < 0) return;
    addr_t m;
    if (!parse_addr(a, ops[2], n == 4 ? ops[3] : 0, &m)) return;
    if (m.rm >= 0) { error(a, "ldrd and strd take an immediate offset"); return; }
    uint32_t u = m.off < 0 ? (uint32_t)-m.off : (uint32_t)m.off;
    if ((u & 3) || u > 1020) { error(a, "the offset must be a multiple of 4 up to 1020"); return; }
    bool up = m.off > 0 || (m.off == 0 && !m.neg);
    uint32_t hw1 = 0xE840u | ((uint32_t)m.pre << 8) | ((uint32_t)up << 7) | ((uint32_t)m.wb << 5)
                 | (in->op == OP_LDRD ? 0x10u : 0) | (uint32_t)m.rn;
    put32w(a, hw1, ((uint32_t)rt << 12) | ((uint32_t)rt2 << 8) | (u >> 2));
}

static void enc_excl(as_t *a, inst_t *in, char **ops, int n)
{
    if (in->op == OP_LDREX) {
        if (!nops(a, n, 2, 2)) return;
        int rt = need_reg(a, ops[0]);
        addr_t m;
        if (rt < 0 || !parse_addr(a, ops[1], 0, &m)) return;
        if (m.rm >= 0 || m.wb || m.off < 0 || (m.off & 3) || m.off > 1020) { error(a, "bad ldrex address"); return; }
        put32w(a, 0xE850u | (uint32_t)m.rn, ((uint32_t)rt << 12) | 0xF00u | ((uint32_t)m.off >> 2));
        return;
    }
    if (!nops(a, n, 3, 3)) return;
    int rd = need_reg(a, ops[0]), rt = need_reg(a, ops[1]);
    addr_t m;
    if (rd < 0 || rt < 0 || !parse_addr(a, ops[2], 0, &m)) return;
    if (m.rm >= 0 || m.wb || m.off < 0 || (m.off & 3) || m.off > 1020) { error(a, "bad strex address"); return; }
    put32w(a, 0xE840u | (uint32_t)m.rn, ((uint32_t)rt << 12) | ((uint32_t)rd << 8) | ((uint32_t)m.off >> 2));
}

static void enc_pushpop(as_t *a, inst_t *in, uint32_t list)
{
    bool push = in->op == OP_PUSH || in->op == OP_STMDB;
    uint32_t extra = push ? (1u << 14) : (1u << 15);
    if (NARROW_OK(in) && (list & ~(0xffu | extra)) == 0) {
        put16(a, (push ? 0xB400u : 0xBC00u) | ((list & extra) ? 0x100u : 0) | (list & 0xff));
        return;
    }
    if (need_narrow_fail(a, in)) return;
    if (list & (1u << 13)) { error(a, "sp cannot be in the list"); return; }
    if (push && (list & (1u << 15))) { error(a, "pc cannot be pushed"); return; }
    if ((list & (list - 1)) == 0) {
        int r = 0;
        while (!(list & (1u << r))) r++;
        if (push) put32w(a, 0xF84Du, ((uint32_t)r << 12) | 0xD04u);
        else      put32w(a, 0xF85Du, ((uint32_t)r << 12) | 0xB04u);
        return;
    }
    put32w(a, push ? 0xE92Du : 0xE8BDu, list);
}

static void enc_ldm(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    uint32_t list;
    if (op == OP_PUSH || op == OP_POP) {
        if (!nops(a, n, 1, 1) || !reglist(a, ops[0], &list)) return;
        enc_pushpop(a, in, list);
        return;
    }
    if (!nops(a, n, 2, 2)) return;
    const char *e;
    int rn = reg_at(ops[0], &e);
    if (rn < 0) { error(a, "'%s' is not a register", ops[0]); return; }
    bool wb = false;
    if (*e == '!') { wb = true; e = skip_ws(e + 1); }
    if (*e) { error(a, "something after the base register"); return; }
    if (!reglist(a, ops[1], &list)) return;
    if (rn == 13 && wb && (op == OP_LDMIA || op == OP_STMDB)) {
        inst_t pp = *in;
        pp.op = op == OP_LDMIA ? OP_POP : OP_PUSH;
        enc_pushpop(a, &pp, list);
        return;
    }
    bool in_list = (list >> rn) & 1;
    if (NARROW_OK(in) && lo(rn) && (list & ~0xffu) == 0) {
        if (op == OP_LDMIA && wb != in_list) {
            put16(a, 0xC800u | ((uint32_t)rn << 8) | list);
            return;
        }
        if (op == OP_STMIA && wb) {
            put16(a, 0xC000u | ((uint32_t)rn << 8) | list);
            return;
        }
    }
    if (need_narrow_fail(a, in)) return;
    uint32_t base = op == OP_LDMIA ? 0xE890u : op == OP_LDMDB ? 0xE910u
                  : op == OP_STMIA ? 0xE880u : 0xE900u;
    put32w(a, base | ((uint32_t)wb << 5) | (uint32_t)rn, list);
}

static const struct { char name[12]; uint8_t sysm; } sysregs[] = {
    { "apsr", 0 }, { "apsr_nzcvq", 0 }, { "iapsr", 1 }, { "eapsr", 2 },
    { "xpsr", 3 }, { "ipsr", 5 }, { "epsr", 6 }, { "iepsr", 7 }, { "msp", 8 },
    { "psp", 9 }, { "msplim", 10 }, { "psplim", 11 }, { "primask", 16 },
    { "basepri", 17 }, { "basepri_max", 18 }, { "faultmask", 19 },
    { "control", 20 },
};

static int sysreg(const char *s)
{
    s = skip_ws(s);
    int n = (int)strlen(s);
    for (size_t i = 0; i < sizeof sysregs / sizeof sysregs[0]; i++)
        if (ieq(s, n, sysregs[i].name)) return sysregs[i].sysm;
    return -1;
}

static void enc_misc(as_t *a, inst_t *in, char **ops, int n)
{
    int op = in->op;
    switch (op) {
    case OP_SVC: case OP_BKPT: case OP_UDF: {
        int32_t v = 0;
        if (!nops(a, n, op == OP_SVC ? 1 : 0, 1)) return;
        if (n && !imm(a, ops[0], &v)) return;
        if (op == OP_UDF && (in->q == 'w' || a->minwide || v > 255)) {
            if (v < 0 || v > 0xffff) { error(a, "%d does not fit", (int)v); return; }
            put32w(a, 0xF7F0u | ((uint32_t)v >> 12), 0xA000u | ((uint32_t)v & 0xfff));
            return;
        }
        if (v < 0 || v > 255) { error(a, "%d does not fit in 8 bits", (int)v); return; }
        put16(a, (op == OP_SVC ? 0xDF00u : op == OP_BKPT ? 0xBE00u : 0xDE00u) | (uint32_t)v);
        return;
    }
    case OP_NOP: case OP_WFI: case OP_WFE: case OP_SEV: case OP_YIELD: {
        if (!nops(a, n, 0, 0)) return;
        int k = op == OP_NOP ? 0 : op == OP_YIELD ? 1 : op == OP_WFE ? 2 : op == OP_WFI ? 3 : 4;
        if (NARROW_OK(in)) { put16(a, 0xBF00u | ((uint32_t)k << 4)); return; }
        put32w(a, 0xF3AFu, 0x8000u | (uint32_t)k);
        return;
    }
    case OP_DSB: case OP_DMB: case OP_ISB: {
        if (!nops(a, n, 0, 1)) return;
        uint32_t opt = 15;
        if (n) {
            const char *s = skip_ws(ops[0]);
            if (*s == '#') { int32_t v; if (!imm(a, s, &v)) return; opt = (uint32_t)v & 15; }
            else if (ieq(s, (int)strlen(s), "sy")) opt = 15;
            else { error(a, "only sy is known as a barrier option"); return; }
        }
        uint32_t k = op == OP_DSB ? 0x40 : op == OP_DMB ? 0x50 : 0x60;
        put32w(a, 0xF3BFu, 0x8F00u | k | opt);
        return;
    }
    case OP_CPSIE: case OP_CPSID: {
        if (!nops(a, n, 1, 1)) return;
        const char *s = skip_ws(ops[0]);
        uint32_t f = 0;
        for (; *s; s++) {
            if (*s == 'i' || *s == 'I') f |= 2;
            else if (*s == 'f' || *s == 'F') f |= 1;
            else { error(a, "cps takes i and f"); return; }
        }
        put16(a, (op == OP_CPSIE ? 0xB660u : 0xB670u) | f);
        return;
    }
    case OP_MRS: {
        if (!nops(a, n, 2, 2)) return;
        int rd = need_reg(a, ops[0]), sm = sysreg(ops[1]);
        if (rd < 0) return;
        if (sm < 0) { error(a, "'%s' is not a special register", ops[1]); return; }
        put32w(a, 0xF3EFu, 0x8000u | ((uint32_t)rd << 8) | (uint32_t)sm);
        return;
    }
    case OP_MSR: {
        if (!nops(a, n, 2, 2)) return;
        int sm = sysreg(ops[0]), rn = need_reg(a, ops[1]);
        if (rn < 0) return;
        if (sm < 0) { error(a, "'%s' is not a special register", ops[0]); return; }
        put32w(a, 0xF380u | (uint32_t)rn, 0x8800u | (uint32_t)sm);
        return;
    }
    case OP_BX: case OP_BLX: {
        if (!nops(a, n, 1, 1)) return;
        int rm = need_reg(a, ops[0]);
        if (rm < 0) return;
        put16(a, (op == OP_BX ? 0x4700u : 0x4780u) | ((uint32_t)rm << 3));
        return;
    }
    case OP_CBZ: case OP_CBNZ: {
        if (!nops(a, n, 2, 2)) return;
        if (in->in_it) { error(a, "cbz and cbnz may not be in an IT block"); return; }
        int rn = need_reg(a, ops[0]);
        int32_t t;
        if (rn < 0 || !target(a, ops[1], &t)) return;
        if (!lo(rn)) { error(a, "cbz and cbnz take r0 to r7"); return; }
        int32_t off = t - (int32_t)(a->addr + 4);
        if (off < 0 || off > 126 || (off & 1)) {
            error(a, "cbz and cbnz reach only 0 to 126 bytes forward");
            off = 0;
        }
        put16(a, (op == OP_CBZ ? 0xB100u : 0xB900u) | (((uint32_t)off >> 6) << 9)
                 | ((((uint32_t)off >> 1) & 0x1f) << 3) | (uint32_t)rn);
        return;
    }
    case OP_TBB: case OP_TBH: {
        if (!nops(a, n, 1, 1)) return;
        addr_t m;
        if (!parse_addr(a, ops[0], 0, &m)) return;
        if (m.rm < 0 || m.wb || m.shift != (op == OP_TBH ? 1 : 0)) {
            error(a, op == OP_TBH ? "tbh takes [rn, rm, lsl #1]" : "tbb takes [rn, rm]");
            return;
        }
        put32w(a, 0xE8D0u | (uint32_t)m.rn, (op == OP_TBH ? 0xF010u : 0xF000u) | (uint32_t)m.rm);
        return;
    }
    }
}

// it, itt, ite, ittee...: the pattern is in the mnemonic and the condition is
// the operand.
static bool try_it(as_t *a, const char *m, int len, char **ops, int n)
{
    if (len < 2 || len > 5 || tolower((unsigned char)m[0]) != 'i' || tolower((unsigned char)m[1]) != 't')
        return false;
    for (int i = 2; i < len; i++) {
        char c = (char)tolower((unsigned char)m[i]);
        if (c != 't' && c != 'e') return false;
    }
    if (a->it_left) { error(a, "an IT block inside an IT block"); return true; }
    if (n != 1) { error(a, "IT needs a condition"); return true; }
    const char *c = skip_ws(ops[0]);
    int fc = cond_of(c, (int)strlen(c));
    if (fc < 0) { error(a, "'%s' is not a condition", c); return true; }
    int k = len - 2;
    if (fc == COND_AL && k) {
        for (int i = 2; i < len; i++)
            if (tolower((unsigned char)m[i]) == 'e') { error(a, "an IT AL block cannot have an else"); return true; }
    }
    uint32_t mask = 0;
    a->it_cond[0] = (uint8_t)fc;
    for (int i = 0; i < k; i++) {
        bool t = tolower((unsigned char)m[2 + i]) == 't';
        uint32_t bit = t ? (uint32_t)(fc & 1) : (uint32_t)((fc & 1) ^ 1);
        mask |= bit << (3 - i);
        a->it_cond[1 + i] = (uint8_t)(t ? fc : (fc ^ 1));
    }
    mask |= 1u << (3 - k);
    put16(a, 0xBF00u | ((uint32_t)fc << 4) | mask);
    a->it_left = k + 1;
    a->it_idx = 0;
    return true;
}

static void instruction(as_t *a, const char *m, int len, char *rest)
{
    char *ops[MAX_OPS];
    int n = split_ops(rest, ops);
    if (n < 0) { error(a, "too many operands"); return; }
    if (try_it(a, m, len, ops, n)) return;
    inst_t in;
    if (!parse_mnem(m, len, &in)) {
        error(a, "'%.*s' is not an instruction", len, m);
        return;
    }
    in.in_it = false;
    bool last_in_it = false;
    if (a->it_left) {
        if (in.cond != a->it_cond[a->it_idx]) {
            error(a, "the condition does not match the IT block");
            a->it_left = 0;
            return;
        }
        in.in_it = true;
        a->it_idx++;
        a->it_left--;
        last_in_it = a->it_left == 0;
    } else if (in.cond != COND_AL && in.op != OP_B) {
        error(a, "a conditional instruction outside an IT block");
        return;
    }
    if (in.in_it && (in.op == OP_B || in.op == OP_BL || in.op == OP_BX || in.op == OP_BLX) && !last_in_it) {
        error(a, "a branch must be last in an IT block");
        return;
    }

    switch (in.op) {
    case OP_MOV: case OP_MVN: enc_mov(a, &in, ops, n); break;
    case OP_MOVW: case OP_MOVT: enc_movwt(a, &in, ops, n); break;
    case OP_ADD: case OP_SUB: case OP_ADDW: case OP_SUBW: enc_addsub(a, &in, ops, n); break;
    case OP_ADC: case OP_SBC: case OP_AND: case OP_ORR: case OP_EOR: case OP_BIC: case OP_ORN:
        enc_logic(a, &in, ops, n); break;
    case OP_RSB: case OP_NEG: enc_rsb(a, &in, ops, n); break;
    case OP_CMP: case OP_CMN: case OP_TST: case OP_TEQ: enc_compare(a, &in, ops, n); break;
    case OP_LSL: case OP_LSR: case OP_ASR: case OP_ROR: case OP_RRX: enc_shift(a, &in, ops, n); break;
    case OP_MUL: case OP_MLA: case OP_MLS: case OP_SDIV: case OP_UDIV:
    case OP_UMULL: case OP_SMULL: case OP_UMLAL: case OP_SMLAL: enc_mul(a, &in, ops, n); break;
    case OP_UXTB: case OP_UXTH: case OP_SXTB: case OP_SXTH: case OP_REV: case OP_REV16:
    case OP_REVSH: case OP_RBIT: case OP_CLZ: enc_extend(a, &in, ops, n); break;
    case OP_BFI: case OP_BFC: case OP_UBFX: case OP_SBFX: enc_bitfield(a, &in, ops, n); break;
    case OP_LDR: case OP_LDRB: case OP_LDRH: case OP_LDRSB: case OP_LDRSH:
    case OP_STR: case OP_STRB: case OP_STRH: enc_ldst(a, &in, ops, n); break;
    case OP_LDRD: case OP_STRD: enc_ldst_d(a, &in, ops, n); break;
    case OP_LDREX: case OP_STREX: enc_excl(a, &in, ops, n); break;
    case OP_LDMIA: case OP_LDMDB: case OP_STMIA: case OP_STMDB: case OP_PUSH: case OP_POP:
        enc_ldm(a, &in, ops, n); break;
    case OP_B: case OP_BL: enc_branch(a, &in, ops, n); break;
    case OP_ADR: {
        if (!nops(a, n, 2, 2)) break;
        int rd = need_reg(a, ops[0]);
        val_t v;
        if (rd < 0 || !expr(a, ops[1], &v)) break;
        if (v.rel != 1) { error(a, "adr takes a label"); break; }
        uint32_t pc = (a->addr + 4) & ~3u;
        int32_t off = v.v - (int32_t)pc;
        if (a->pass == 0) v.undef = true;     // short in the first layout
        if (NARROW_OK(&in) && lo(rd)
            && (v.undef || (v.est ? off <= 1020 : (off >= 0 && off <= 1020 && (off & 3) == 0)))) {
            put16(a, 0xA000u | ((uint32_t)rd << 8) | (v.undef || off < 0 ? 0 : ((uint32_t)off >> 2) & 0xff));
            break;
        }
        if (need_narrow_fail(a, &in)) break;
        if (off < -4095 || off > 4095) {
            if (v.est) off = 0; else { error(a, "the label is out of adr's reach"); break; }
        }
        if (off >= 0) put_imm12(a, 0xF20Fu, (uint32_t)rd << 8, (uint32_t)off);
        else          put_imm12(a, 0xF2AFu, (uint32_t)rd << 8, (uint32_t)-off);
        break;
    }
    default: enc_misc(a, &in, ops, n); break;
    }
}

// --- directives ------------------------------------------------------------

static void emit_pool(as_t *a)
{
    if (a->nlit == a->pool_first) { a->pool_no++; return; }
    note_align(a, a->cur_pos + 1, 2);
    while (a->addr & 3) put8(a, 0);
    if (a->pool_no >= a->npool_cap) {
        int c = a->npool_cap ? a->npool_cap * 2 : 8;
        while (c <= a->pool_no) c *= 2;
        uint32_t *pa = realloc(a->pool_addr, (size_t)c * sizeof *pa);
        bool *pk = realloc(a->pool_known, (size_t)c * sizeof *pk);
        int *pp = realloc(a->pool_pos, (size_t)c * sizeof *pp);
        if (pa) a->pool_addr = pa;
        if (pk) a->pool_known = pk;
        if (pp) a->pool_pos = pp;
        if (!pa || !pk || !pp) { error(a, "out of memory"); return; }
        for (int i = a->npool_cap; i < c; i++) { a->pool_addr[i] = 0; a->pool_known[i] = false; a->pool_pos[i] = 0; }
        a->npool_cap = c;
    }
    if (!a->pool_known[a->pool_no] || a->pool_addr[a->pool_no] != a->addr) a->changed = true;
    a->pool_addr[a->pool_no] = a->addr;
    a->pool_known[a->pool_no] = true;
    a->pool_pos[a->pool_no] = a->cur_pos + 1;
    for (int k = a->pool_first; k < a->nlit; k++) {
        val_t v = { a->lit[k].v, a->lit[k].rel, false, a->lit[k].func, false };
        put_word(a, v);
    }
    a->pool_first = a->nlit;
    a->pool_no++;
}

static bool string_lit(as_t *a, const char *s, bool zero)
{
    s = skip_ws(s);
    if (*s != '"') { error(a, "a string in double quotes was expected"); return false; }
    s++;
    while (*s && *s != '"') {
        int c = char_escape(&s);
        put8(a, (uint8_t)c);
    }
    if (*s != '"') { error(a, "the string has no end"); return false; }
    if (*skip_ws(s + 1)) { error(a, "something after the string"); return false; }
    if (zero) put8(a, 0);
    return true;
}

static void align_to(as_t *a, uint32_t n, bool have_fill, int32_t fill)
{
    if (n == 0 || (n & (n - 1))) { error(a, "the alignment must be a power of two"); return; }
    if (have_fill) {
        while (a->addr & (n - 1)) put8(a, (uint8_t)fill);
        return;
    }
    // Code is padded with no-ops, as GNU as does, so that falling through the
    // gap is harmless: a zero byte to reach a halfword, one short nop to reach
    // a word, and nop.w after that.
    if ((a->addr & 1) && (a->addr & (n - 1))) put8(a, 0);
    if ((a->addr & 2) && (a->addr & (n - 1))) put16(a, 0xBF00u);
    while (a->addr & (n - 1)) put32w(a, 0xF3AFu, 0x8000u);
}

static void one_line(as_t *a, const char *src, int len);

static struct incfile *load_file(as_t *a, const char *path)
{
    for (int i = 0; i < a->nfiles; i++)
        if (!strcmp(a->files[i].path, path)) return &a->files[i];
    long size;
    char *t = a->inc->load(a->inc->ctx, path, &size);
    if (!t) return 0;
    struct incfile *nf = realloc(a->files, (size_t)(a->nfiles + 1) * sizeof *nf);
    if (!nf) { free(t); return 0; }
    a->files = nf;
    struct incfile *f = &a->files[a->nfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->text = t;
    int cap = 0;
    char *p = t;
    while (*p) {
        char *e = strchr(p, '\n');
        if (f->nlines == cap) {
            cap = cap ? cap * 2 : 64;
            f->lines = realloc(f->lines, (size_t)cap * sizeof *f->lines);
            f->lens = realloc(f->lens, (size_t)cap * sizeof *f->lens);
            if (!f->lines || !f->lens) { f->nlines = 0; return f; }
        }
        f->lines[f->nlines] = p;
        f->lens[f->nlines] = e ? (int)(e - p) : (int)strlen(p);
        f->nlines++;
        if (!e) break;
        p = e + 1;
    }
    return f;
}

// .include "name": the lines of another file, assembled here as though they
// were written in place.
static void include_file(as_t *a, const char *arg)
{
    const char *s = skip_ws(arg);
    if (*s != '"') { error(a, ".include wants a name in double quotes"); return; }
    char name[160];
    int n = 0;
    s++;
    while (*s && *s != '"' && n < (int)sizeof name - 1) name[n++] = *s++;
    name[n] = 0;
    if (*s != '"' || !n) { error(a, ".include wants a name in double quotes"); return; }
    if (!a->inc || !a->inc->load) { error(a, "files cannot be included here"); return; }
    if (a->depth >= 7) { error(a, "includes nested more than seven deep"); return; }

    // Beside the file that includes it, then beside the source (where GNU as
    // would look), then as written.
    char path[192];
    struct incfile *f = 0;
    for (int where = 0; where < 2 && !f && name[0] != '/'; where++) {
        const char *dir;
        int dl;
        if (where == 0 && a->depth) {
            dir = a->inc_path[a->depth];
            const char *sl = strrchr(dir, '/');
            dl = sl ? (int)(sl - dir) : 0;
        } else if (where == 1 || !a->depth) {
            dir = a->inc->dir ? a->inc->dir : "";
            dl = (int)strlen(dir);
            if (where == 0) where = 1;
        } else continue;
        if (dl) {
            snprintf(path, sizeof path, dir[dl - 1] == '/' ? "%.*s%s" : "%.*s/%s", dl, dir, name);
            f = load_file(a, path);
        }
    }
    if (!f) {
        snprintf(path, sizeof path, "%s", name);
        f = load_file(a, path);
    }
    if (!f) { error(a, "cannot open %s", name); return; }
    for (int d = 1; d <= a->depth; d++)
        if (!strcmp(a->inc_path[d], f->path)) { error(a, "%s includes itself", name); return; }

    int k = a->inst_no++;
    if (k >= a->ninsts) {
        struct incinst *ni = realloc(a->insts, (size_t)(k + 1) * sizeof *ni);
        if (!ni) { error(a, "out of memory"); return; }
        a->insts = ni;
        for (int i = a->ninsts; i <= k; i++) {
            a->insts[i].file = -1;
            a->insts[i].sizes = 0;
            a->insts[i].addrs = 0;
            a->insts[i].frozen = 0;
        }
        a->ninsts = k + 1;
    }
    int fi = (int)(f - a->files);
    if (a->insts[k].file != fi) {
        free(a->insts[k].sizes);
        free(a->insts[k].addrs);
        free(a->insts[k].frozen);
        a->insts[k].file = fi;
        size_t nl = (size_t)(f->nlines ? f->nlines : 1);
        a->insts[k].sizes = calloc(nl, sizeof(uint32_t));
        a->insts[k].addrs = calloc(nl, sizeof(uint32_t));
        a->insts[k].frozen = calloc(nl, sizeof(bool));
        a->changed = true;
    }
    uint32_t *sizes = a->insts[k].sizes, *addrs = a->insts[k].addrs;
    bool *frozen = a->insts[k].frozen;
    if (!sizes || !addrs || !frozen) { error(a, "out of memory"); return; }

    int saved_id = a->line_id, saved_pos = a->cur_pos;
    uint32_t saved_prev = a->prev_addr, saved_start = a->line_start;
    a->depth++;
    snprintf(a->inc_path[a->depth], sizeof a->inc_path[0], "%s", f->path);
    for (int j = 0; j < f->nlines; j++) {
        a->inc_line[a->depth] = j;
        a->line_id = ((k + 1) << 16) | j;
        a->line_failed = false;
        a->minwide = frozen[j] || (a->sticky && sizes[j] >= 4);
        a->prev_addr = addrs[j];
        a->line_start = a->addr;
        a->cur_pos = 4 * a->seq++ + 1;
        a->was_inst = false;
        uint32_t start = a->addr;
        one_line(a, f->lines[j], f->lens[j]);
        uint32_t size = a->addr - start;
        if (a->was_inst && size == 4 && (int32_t)(start - addrs[j]) <= 0 && a->pass > 0) frozen[j] = true;
        addrs[j] = start;
        if (size != sizes[j]) a->changed = true;
        sizes[j] = size;
    }
    a->depth--;
    a->line_id = saved_id;
    a->cur_pos = saved_pos;
    a->prev_addr = saved_prev;
    a->line_start = saved_start;
    a->line_failed = false;
    a->was_inst = false;
}

static bool directive(as_t *a, const char *d, int len, char *rest)
{
    char *ops[MAX_OPS];
#define IS(x) ieq(d, len, x)
    if (IS(".syntax") || IS(".thumb") || IS(".text") || IS(".global") || IS(".globl")
        || IS(".type") || IS(".size") || IS(".code") || IS(".section") || IS(".cpu")
        || IS(".fpu") || IS(".arch") || IS(".file") || IS(".end") || IS(".eabi_attribute")
        || IS(".ident") || IS(".weak") || IS(".arm_func") || IS(".fnstart") || IS(".fnend")
        || IS(".cantunwind") || IS(".data"))
        return true;
    if (IS(".thumb_func")) { a->next_func = true; return true; }
    if (IS(".ltorg") || IS(".pool")) { emit_pool(a); return true; }
    if (IS(".ascii") || IS(".asciz") || IS(".string")) {
        int n = split_ops(rest, ops);
        if (n < 1) { error(a, "a string was expected"); return true; }
        for (int i = 0; i < n; i++)
            if (!string_lit(a, ops[i], !IS(".ascii"))) break;
        return true;
    }
    if (IS(".byte") || IS(".hword") || IS(".short") || IS(".2byte") || IS(".word")
        || IS(".long") || IS(".4byte") || IS(".int")) {
        int size = IS(".byte") ? 1 : (IS(".hword") || IS(".short") || IS(".2byte")) ? 2 : 4;
        int n = split_ops(rest, ops);
        if (n < 1) { error(a, "a value was expected"); return true; }
        for (int i = 0; i < n; i++) {
            val_t v;
            if (!expr(a, ops[i], &v)) return true;
            if (size == 4) { put_word(a, v); continue; }
            if (v.rel) { error(a, "an address does not fit in %d bytes", size); return true; }
            if (size == 1 && (v.v < -128 || v.v > 255)) { error(a, "%d does not fit in a byte", (int)v.v); return true; }
            if (size == 2 && (v.v < -32768 || v.v > 65535)) { error(a, "%d does not fit in a halfword", (int)v.v); return true; }
            put8(a, (uint8_t)v.v);
            if (size == 2) put8(a, (uint8_t)(v.v >> 8));
        }
        return true;
    }
    if (IS(".space") || IS(".skip") || IS(".zero")) {
        int n = split_ops(rest, ops);
        int32_t count, fill = 0;
        if (n < 1 || n > 2 || !imm(a, ops[0], &count) || (n == 2 && !imm(a, ops[1], &fill))) {
            if (!a->line_failed) error(a, "how many bytes?");
            return true;
        }
        if (count < 0 || count > 1 << 20) { error(a, "%d bytes is out of range", (int)count); return true; }
        for (int32_t i = 0; i < count; i++) put8(a, (uint8_t)fill);
        return true;
    }
    if (IS(".align") || IS(".p2align") || IS(".balign")) {
        int n = split_ops(rest, ops);
        int32_t v = 2, fill = 0;
        if (n >= 1 && !imm(a, ops[0], &v)) return true;
        if (n >= 2 && !imm(a, ops[1], &fill)) return true;
        if (IS(".balign")) {
            int pow = 0;
            while (pow < 16 && (1 << pow) < v) pow++;
            note_align(a, a->cur_pos, pow);
            align_to(a, (uint32_t)v, n >= 2, fill);
        } else {
            if (v < 0 || v > 16) { error(a, "the alignment is out of range"); return true; }
            note_align(a, a->cur_pos, (int)v);
            align_to(a, 1u << v, n >= 2, fill);
        }
        return true;
    }
    if (IS(".equ") || IS(".set")) {
        int n = split_ops(rest, ops);
        if (n != 2) { error(a, "%.*s name, value", len, d); return true; }
        const char *nm = skip_ws(ops[0]);
        int k = 0;
        while (is_id(nm[k])) k++;
        if (!k || nm[k] || !is_id0((unsigned char)nm[0])) { error(a, "'%s' is not a name", nm); return true; }
        val_t v;
        if (!expr(a, ops[1], &v)) return true;
        define(a, nm, k, v.v, v.rel, true);
        return true;
    }
    if (IS(".include")) {
        include_file(a, rest);
        return true;
    }
    if (IS(".stack")) {
        int32_t v;
        if (!imm(a, rest, &v)) return true;
        if (v < 512 || v > 1 << 20) { error(a, "a stack of %d bytes is out of range", (int)v); return true; }
        a->r->stack = (uint32_t)v;
        return true;
    }
#undef IS
    error(a, "'%.*s' is not a directive", len, d);
    return true;
}

// --- lines -----------------------------------------------------------------

static void one_line(as_t *a, const char *src, int len)
{
    char buf[LINE_MAX_LEN];
    if (len >= LINE_MAX_LEN) { error(a, "the line is too long"); return; }
    // Copy without the comment: @, ; or //, outside quotes.
    int j = 0;
    char q = 0;
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (q) {
            buf[j++] = c;
            if (c == '\\' && i + 1 < len) { buf[j++] = src[++i]; continue; }
            if (c == q) q = 0;
            continue;
        }
        if (c == '"') q = c;
        else if (c == '\'' ) {
            // A character constant: copy it whole so a quote inside is not taken
            // for a comment or a string.
            buf[j++] = c;
            if (i + 1 < len && src[i + 1] == '\\' && i + 2 < len) { buf[j++] = src[++i]; buf[j++] = src[++i]; }
            else if (i + 1 < len) buf[j++] = src[++i];
            if (i + 1 < len && src[i + 1] == '\'') buf[j++] = src[++i];
            continue;
        }
        else if (c == '@' || c == ';' || (c == '/' && i + 1 < len && src[i + 1] == '/')) break;
        buf[j++] = c;
    }
    buf[j] = 0;

    char *p = (char *)skip_ws(buf);
    // Labels, any number of them: "name:".
    for (;;) {
        int k = 0;
        if (isdigit((unsigned char)p[0])) {
            while (isdigit((unsigned char)p[k])) k++;
            if (p[k] != ':' || k > 3) break;
            int nlab = atoi(p);
            char nm[16];
            int len = snprintf(nm, sizeof nm, "%d:%d", nlab, a->local_count[nlab]++);
            define(a, nm, len, (int32_t)a->addr, 1, false);
            p = (char *)skip_ws(p + k + 1);
            continue;
        }
        if (!is_id0((unsigned char)p[0])) break;
        while (is_id(p[k])) k++;
        if (p[k] != ':') break;
        define(a, p, k, (int32_t)a->addr, 1, false);
        p = (char *)skip_ws(p + k + 1);
    }
    if (!*p) return;

    int k = 0;
    while (is_id(p[k])) k++;
    if (!k) { error(a, "cannot read this line"); return; }
    char *rest = (char *)skip_ws(p + k);
    // name = value
    if (*rest == '=' && rest[1] != '=') {
        val_t v;
        if (expr(a, rest + 1, &v)) define(a, p, k, v.v, v.rel, true);
        return;
    }
    if (p[0] == '.') { directive(a, p, k, rest); return; }
    // Thumb instructions are on halfwords; data before them may have left the
    // address odd.
    if (a->addr & 1) { error(a, "an instruction on an odd address; add .align 1"); return; }
    instruction(a, p, k, rest);
    a->was_inst = true;
}

static void run_pass(as_t *a)
{
    a->addr = 0;
    a->changed = false;
    a->nrel = 0;
    a->nlit = 0;
    a->pool_first = 0;
    a->pool_no = 0;
    a->it_left = 0;
    a->next_func = false;
    memset(a->local_count, 0, sizeof a->local_count);
    a->inst_no = 0;
    a->depth = 0;
    a->seq = 0;
    // This pass's alignments are collected afresh; the last pass's are what
    // moves are rounded by.
    struct align *t = a->al_prev; a->al_prev = a->al; a->al = t;
    int tc = a->alcap_prev; a->alcap_prev = a->alcap; a->alcap = tc;
    a->nal_prev = a->nal;
    a->nal = 0;
    asm_result_t *r = a->r;
    r->stack = 0;
    for (int i = 0; i < a->nlines; i++) {
        int len;
        const char *s = a->get(a->ctx, i, &len);
        a->line = i;
        a->line_id = i;
        a->line_failed = false;
        a->minwide = a->frozen[i] || (a->sticky && r->line_size[i] >= 4);
        a->prev_addr = r->line_addr[i];
        a->line_start = a->addr;
        a->cur_pos = 4 * a->seq++ + 1;
        a->was_inst = false;
        uint32_t start = a->addr;
        int it_before = a->it_left;
        one_line(a, s, len);
        uint32_t size = a->addr - start;
        // Long while nothing before it grew in this pass: long for good.
        if (a->was_inst && size == 4 && (int32_t)(start - r->line_addr[i]) <= 0 && a->pass > 0)
            a->frozen[i] = true;
        if (r->line_addr[i] != start) a->changed = true;
        r->line_addr[i] = start;
        (void)it_before;
        if (size != r->line_size[i]) a->changed = true;
        r->line_size[i] = size;
    }
    if (a->it_left) {
        a->line = a->nlines ? a->nlines - 1 : 0;
        a->line_failed = false;
        error(a, "the IT block is not finished");
    }
    a->line_failed = false;
    a->cur_pos = 4 * a->seq + 1;
    emit_pool(a);
}

bool asm_assemble(asm_line_fn get, void *ctx, int nlines, const asm_include_t *inc,
                  asm_result_t *r)
{
    memset(r, 0, sizeof *r);
    as_t a;
    memset(&a, 0, sizeof a);
    a.get = get;
    a.ctx = ctx;
    a.nlines = nlines;
    a.r = r;
    a.inc = inc;
    for (int i = 0; i < 256; i++) a.bucket[i] = -1;
    r->nlines = nlines;
    r->line_addr = calloc((size_t)(nlines ? nlines : 1), sizeof *r->line_addr);
    r->line_size = calloc((size_t)(nlines ? nlines : 1), sizeof *r->line_size);
    a.frozen = calloc((size_t)(nlines ? nlines : 1), sizeof *a.frozen);
    if (!r->line_addr || !r->line_size || !a.frozen) {
        free(a.frozen);
        r->nerrors = 1;
        strcpy(r->errors[0].msg, "out of memory");
        return false;
    }

    // Passes until nothing moves, then one in which nothing is estimated and
    // the errors count. If that one still changes something -- a load whose
    // literal turned out not to be aligned -- the passes go on.
    int pass = 0;
    for (;;) {
        a.sticky = pass >= STICKY_AFTER;
        a.pass = pass++;
        a.final = false;
        run_pass(&a);
        if ((a.changed || pass == 1) && pass < MAX_PASSES) continue;
        a.pass = pass++;
        a.final = true;
        r->nerrors = 0;
        run_pass(&a);
        if (!a.changed || pass >= MAX_PASSES) break;
    }
    r->passes = pass;

    r->code = a.buf;
    r->size = a.addr;
    r->relocs = a.rel;
    r->nrelocs = a.nrel;
    r->entry = 0;
    sym_t *m = sym_find(&a, "main", 4, false);
    if (!m || m->defpass < 0 || m->rel != 1) m = sym_find(&a, "_start", 6, false);
    if (m && m->defpass >= 0 && m->rel == 1) r->entry = (uint32_t)m->value;
    for (int i = 0; i < a.nfiles; i++) { free(a.files[i].text); free(a.files[i].lines); free(a.files[i].lens); }
    free(a.files);
    for (int i = 0; i < a.ninsts; i++) { free(a.insts[i].sizes); free(a.insts[i].addrs); free(a.insts[i].frozen); }
    free(a.frozen);
    free(a.al);
    free(a.al_prev);
    free(a.pool_pos);
    free(a.insts);
    free(a.syms);
    free(a.lit);
    free(a.pool_addr);
    free(a.pool_known);
    return r->nerrors == 0;
}

void asm_free(asm_result_t *r)
{
    free(r->code);
    free(r->relocs);
    free(r->line_addr);
    free(r->line_size);
    memset(r, 0, sizeof *r);
}
