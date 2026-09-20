#include "../../common/ubiqos_abi.h"

// key -- what the board keeps secret.
//
//   key                 the names, and how long each value is
//   key set NAME        type the value; it is not shown
//   key remove NAME
//   key check NAME      a fingerprint, to compare with the one you have
//
// The value is typed here and nowhere else: not as an argument, because argv
// is a process's memory and the shell keeps sixteen lines of history; not
// echoed, because a terminal keeps a scrollback and a screenshot keeps
// everything. And it never comes back out -- there is no `key get`, and the
// kernel has no call that would answer one.
//
// What a fingerprint is for: to tell whether the board holds the key you think
// it does, without either of you saying which. It is a CRC of the value, four
// bytes, the same on the board as in the little script beside your password
// manager.

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    while (i < UBIQOS_KEY_NAME_MAX - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void list(void) {
    ubiqos_keyreq_t r;
    const int32_t n = ubiqos_key_op(UBIQOS_KEY_OP_COUNT, &r);
    if (n <= 0) { say("no keys stored\r\n"); return; }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "name                      bytes\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
    for (int32_t i = 0; i < n; i++) {
        r.index = (uint32_t)i;
        if (ubiqos_key_op(UBIQOS_KEY_OP_NTH, &r) != 0) continue;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, r.name);
        for (uint32_t k = 0; k < UBIQOS_KEY_NAME_MAX + 2; k++) {
            if (!r.name[k]) { while (k < UBIQOS_KEY_NAME_MAX + 2) { ubiqos_line_str(&l, " "); k++; } break; }
        }
        ubiqos_line_u32(&l, r.len);
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}

// The value, read a character at a time and never drawn. Ctrl-C abandons it.
static uint32_t ask(uint8_t *out, uint32_t cap) {
    say("value: ");
    uint32_t n = 0;
    for (;;) {
        uint8_t ch;
        if (ubiqos_read(UBIQOS_STDIN, &ch, 1) <= 0) continue;
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3) { n = 0; break; }
        if (ch == 8 || ch == 127) { if (n) n--; continue; }
        if (ch >= ' ' && n < cap) out[n++] = ch;
    }
    say("\r\n");
    return n;
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: key [set NAME | remove NAME | check NAME]\n\n"
            "  (none)        the names of the keys stored, and their lengths\n"
            "  set NAME      type the value; it is not shown and never an argument\n"
            "  remove NAME   forget it\n"
            "  check NAME    a fingerprint of the value, to compare with your own\n\n"
            "Values cannot be read back. They are kept in flash, outside anything\n"
            "a system update writes, and the kernel uses them on a program's\n"
            "behalf rather than handing them over.\n"))
        return;

    if (argc == 1) { list(); return; }
    const bool set = argc == 3 && is(argv[1], "set");
    const bool rem = argc == 3 && is(argv[1], "remove");
    const bool chk = argc == 3 && is(argv[1], "check");
    if (!set && !rem && !chk) { say("usage: key [set NAME | remove NAME | check NAME]\r\n"); return; }

    ubiqos_keyreq_t r;
    for (uint32_t i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
    copy_name(r.name, argv[2]);

    if (chk) {
        if (ubiqos_key_op(UBIQOS_KEY_OP_PRINT, &r) != 0) { say("key: no such key\r\n"); return; }
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "fingerprint ");
        ubiqos_line_hex(&l, r.fingerprint);
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        return;
    }

    if (set) {
        r.len = ask(r.value, sizeof r.value);
        if (!r.len) { say("nothing typed\r\n"); return; }
    }

    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_SET, &r);
    // Whatever happens, the value does not stay in this process's memory a
    // moment longer than it must.
    for (uint32_t i = 0; i < sizeof r.value; i++) r.value[i] = 0;

    if (rc == 0)       say(set ? "stored\r\n" : "removed\r\n");
    else if (rc == -1) say("key: no such key, or the value is too long\r\n");
    else if (rc == -2) say("key: the store is full\r\n");
    else if (rc == -3) say("key: the other core would not stand still; try again\r\n");
    else               say("key: the write did not take\r\n");
}
