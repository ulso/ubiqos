#include "../../common/ubiqos_abi.h"

// key -- what the board keeps secret.
//
//   key unlock          type the passphrase; the store opens until the power goes
//   key lock            forget it again
//   key                 the names, and how long each value is
//   key set NAME        type the value; it is not shown
//   key remove NAME
//   key check NAME      a fingerprint, to compare with the one you have
//
// The store is sealed: locked, there is nothing to list, because the names are
// inside the ciphertext with the values. The first unlock on a board with no
// store makes one, and what is typed then is the passphrase from then on.
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

// The passphrase or a value: read a character at a time, never drawn.
// Ctrl-C abandons it.
static uint32_t ask(const char *what, uint8_t *out, uint32_t cap) {
    say(what);
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

static bool locked(void) {
    ubiqos_keyreq_t r;
    const int32_t st = ubiqos_key_op(UBIQOS_KEY_OP_STATE, &r);
    if (st == (int32_t)UBIQOS_KEYS_OPEN) return false;
    say(st == (int32_t)UBIQOS_KEYS_EMPTY
            ? "no store yet -- 'key unlock' makes one\r\n"
            : "the store is locked -- 'key unlock' opens it\r\n");
    return true;
}

// Is a program of this name already running? Unlocking twice should not start
// a second server, and `ps` is the only one who knows.
static bool already_running(const char *name) {
    for (uint32_t i = 0; i < UBIQOS_PS_SLOTS; i++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(i, &p) != 0) continue;
        if (p.state != UBIQOS_PS_ZOMBIE && is(p.name, name)) return true;
    }
    return false;
}

// Is there a key of this name? The store answers with a fingerprint, which is
// not the value, and a failure means there is no such key.
static bool have_key(const char *name) {
    ubiqos_keyreq_t r;
    for (uint32_t i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
    uint32_t n = 0;
    while (name[n] && n < UBIQOS_KEY_NAME_MAX - 1) { r.name[n] = name[n]; n++; }
    r.name[n] = 0;
    return ubiqos_key_op(UBIQOS_KEY_OP_PRINT, &r) == 0;
}

// The shell over the network, if this board is one that answers SSH.
//
// sshd cannot start at boot: its host key is derived from the store and the
// password is in it, so a locked board has neither. Unlocking is the moment
// both appear, and it is also the moment somebody is demonstrably present --
// which is the right condition for opening a way in.
//
// Only when there is a password to check against. Without one nobody could log
// in, and a server listening for logins it must refuse is not a service.
static void serve_ssh(void) {
    if (!have_key("ssh.password") || already_running("sshd")) return;
    // Not waited for: it runs until killed. But the answer IS looked at -- a
    // server that failed to start and said nothing is a port that is simply
    // refused later, with nowhere to look.
    if (ubiqos_exec("sshd", "") < 0)
        say("key: there is a password for ssh but sshd would not start\r\n");
}

// A store with a network's password in it is nearly always unlocked BECAUSE of
// that password: the clock wants the network and so does anything that calls
// out, and forgetting to ask for it afterwards is how a board sits there with
// the right key and no link.
//
// So `wifi auto` is RUN here -- the command, as a person would type it. There
// is no radio code in this program and nothing links the two: a machine
// without `wifi` has nothing to run, and nothing happens. What comes out on
// the screen is that command's own account of itself, which is the difference
// between doing something for somebody and doing something behind their back.
//
// Only when the store actually holds a network's password. A board whose keys
// are all for web services has no business turning a radio on.
static void join_if_networks(void) {
    ubiqos_keyreq_t r;
    const int32_t n = ubiqos_key_op(UBIQOS_KEY_OP_COUNT, &r);
    bool any = false;
    for (int32_t i = 0; i < n && !any; i++) {
        r.index = (uint32_t)i;
        if (ubiqos_key_op(UBIQOS_KEY_OP_NTH, &r) != 0) continue;
        const char *p = r.name;
        for (const char *k = "wifi."; *k; k++) { if (*p != *k) { p = 0; break; } p++; }
        if (p && *p) any = true;
    }
    if (!any) return;

    const int32_t pid = ubiqos_exec("wifi", "auto");
    if (pid >= 0) ubiqos_wait(pid);
}

static void unlock(void) {
    ubiqos_keyreq_t r;
    for (uint32_t i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
    const int32_t st = ubiqos_key_op(UBIQOS_KEY_OP_STATE, &r);
    if (st == (int32_t)UBIQOS_KEYS_OPEN) { say("already unlocked\r\n"); return; }
    if (st == (int32_t)UBIQOS_KEYS_EMPTY)
        say("no store yet: what you type now becomes the passphrase.\r\n");

    r.len = ask("passphrase: ", r.value, sizeof r.value);
    if (!r.len) { say("nothing typed\r\n"); return; }
    say("working");                       // PBKDF2 takes about a second
    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_UNLOCK, &r);
    for (uint32_t i = 0; i < sizeof r.value; i++) r.value[i] = 0;
    say("\r\n");

    if (rc == 0 || rc == 1) {
        say(rc ? "a new store, unlocked\r\n" : "unlocked\r\n");
        join_if_networks();     // the network first: sshd waits for a stack
        serve_ssh();
        return;
    }
    if (rc == -6)      say("key: that is not the passphrase, or the store has been changed\r\n");
    else if (rc == -3) say("key: the other core would not stand still; try again\r\n");
    else               say("key: could not open the store\r\n");
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

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: key [unlock | lock | set NAME | remove NAME | check NAME]\n\n"
            "  unlock        type the passphrase; on a board with no store yet,\n"
            "                what you type becomes the passphrase. Afterwards:\n"
            "                'wifi auto' if the store holds a network's password,\n"
            "                and sshd if it holds one under 'ssh.password' --\n"
            "                which is what it was unlocked for\n"
            "  lock          forget it; the store is sealed again\n"
            "  destroy       erase the store: every key and the passphrase\n"
            "  (none)        the names of the keys stored, and their lengths\n"
            "  set NAME      type the value; it is not shown and never an argument\n"
            "  remove NAME   forget it\n"
            "  check NAME    a fingerprint of the value, to compare with your own\n\n"
            "Values cannot be read back. The store is sealed in flash, outside\n"
            "anything a system update writes, and is opened by a passphrase --\n"
            "which must be typed again after every power-up, because the key\n"
            "derived from it is kept only in RAM.\n"))
        return;

    if (argc == 2 && is(argv[1], "unlock")) { unlock(); return; }
    if (argc == 2 && is(argv[1], "destroy")) {
        ubiqos_keyreq_t r;
        for (uint32_t i = 0; i < sizeof r; i++) ((uint8_t *)&r)[i] = 0;
        say("This erases every key and the passphrase. Type 'destroy' to go on: ");
        uint8_t answer[16];
        uint32_t n = 0;
        for (;;) {
            uint8_t ch;
            if (ubiqos_read(UBIQOS_STDIN, &ch, 1) <= 0) continue;
            if (ch == '\r' || ch == '\n') break;
            if (ch == 3) { n = 0; break; }
            if (ch >= ' ' && n < sizeof answer - 1) { answer[n++] = ch; ubiqos_write(UBIQOS_STDOUT, &ch, 1); }
        }
        answer[n] = 0;
        say("\r\n");
        if (!is((const char *)answer, "destroy")) { say("left alone\r\n"); return; }
        say(ubiqos_key_op(UBIQOS_KEY_OP_DESTROY, &r) == 0 ? "destroyed\r\n"
                                                          : "key: the erase did not take\r\n");
        return;
    }
    if (argc == 2 && is(argv[1], "lock")) {
        ubiqos_keyreq_t r;
        ubiqos_key_op(UBIQOS_KEY_OP_LOCK, &r);
        say("locked\r\n");
        return;
    }
    if (argc == 1) { if (!locked()) list(); return; }
    const bool set = argc == 3 && is(argv[1], "set");
    const bool rem = argc == 3 && is(argv[1], "remove");
    const bool chk = argc == 3 && is(argv[1], "check");
    if (!set && !rem && !chk) {
        say("usage: key [unlock | lock | destroy | set NAME | remove NAME | check NAME]\r\n");
        return;
    }
    if (locked()) return;

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
        r.len = ask("value: ", r.value, sizeof r.value);
        if (!r.len) { say("nothing typed\r\n"); return; }
    }

    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_SET, &r);
    // Whatever happens, the value does not stay in this process's memory a
    // moment longer than it must.
    for (uint32_t i = 0; i < sizeof r.value; i++) r.value[i] = 0;

    if (rc == 0)       say(set ? "stored\r\n" : "removed\r\n");
    else if (rc == -5) say("key: the store is locked\r\n");
    else if (rc == -1) say("key: no such key, or the value is too long\r\n");
    else if (rc == -2) say("key: the store is full\r\n");
    else if (rc == -3) say("key: the other core would not stand still; try again\r\n");
    else               say("key: the write did not take\r\n");
}
