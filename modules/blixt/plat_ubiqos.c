// Blixt on the board.
#include "plat.h"
#include "modwrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/ubiqos_abi.h"

// A quarter of a megabyte for the text, the code and the listing, from PSRAM.
uint32_t ubiqos_heap_bytes = 256u * 1024u;
UBIQOS_MEM_SIZE(16384);

#define PULSE_INTR  0x6249u     // "Ib": Ctrl-C on the board's keyboard
#define PULSE_CHILD 0x6243u     // "Cb": the program that was run has ended

void plat_init(void)
{
    // Ctrl-C on the board's own keyboard would end the editor and lose the
    // text. It is caught, and is then an ordinary key that does nothing.
    ubiqos_catch_intr(PULSE_INTR);
}

void plat_done(void)
{
    ubiqos_catch_intr(0);
}

void plat_write(const char *s, int n)
{
    ubiqos_write(UBIQOS_STDOUT, s, (uint32_t)n);
}

// Pulses this program did not ask to wait for -- a caught Ctrl-C -- would
// otherwise sit in the kernel's ring.
static void drain_pulses(void)
{
    ubiqos_msg_t m;
    while (ubiqos_receive_tmo(&m, 0) >= 0) {
        if (m.sender > 0) ubiqos_reply(-1);
    }
}

int plat_read(uint8_t *buf, int max, int timeout_ms)
{
    uint32_t start = ubiqos_ticks_now();
    for (;;) {
        int32_t n = ubiqos_readable(UBIQOS_STDIN);
        if (n > 0) {
            if (n > max) n = max;
            int32_t got = ubiqos_read(UBIQOS_STDIN, buf, (uint32_t)n);
            return got > 0 ? (int)got : 0;
        }
        if (n < 0) {
            // No way to ask; a plain read blocks until there is a byte.
            int32_t got = ubiqos_read(UBIQOS_STDIN, buf, 1);
            return got > 0 ? (int)got : 0;
        }
        if (timeout_ms >= 0 && (int32_t)(ubiqos_ticks_now() - start) >= timeout_ms) return 0;
        drain_pulses();
        // Two milliseconds between looks while a key sequence is arriving;
        // ten while nothing is happening, which costs nothing anyone sees.
        ubiqos_sleep(timeout_ms >= 0 ? 2 : 10);
    }
}

uint32_t plat_ms(void) { return ubiqos_ticks_now(); }

void plat_default_size(int *rows, int *cols)
{
    ubiqos_confont_t f;
    if (ubiqos_console_font_info(-1, &f) >= 0 && f.rows && f.cols) {
        *rows = f.rows;
        *cols = f.cols;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

char *plat_load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long cap = 4096, n = 0;
    char *b = malloc((size_t)cap + 1);
    for (;;) {
        if (!b) { fclose(f); return 0; }
        size_t got = fread(b + n, 1, (size_t)(cap - n), f);
        n += (long)got;
        if (n < cap) break;
        cap *= 2;
        char *p = realloc(b, (size_t)cap + 1);
        if (!p) { free(b); fclose(f); return 0; }
        b = p;
    }
    fclose(f);
    b[n] = 0;
    *size = n;
    return b;
}

bool plat_save(const char *path, const char *data, long size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)size, f) == (size_t)size;
    return fclose(f) == 0 && ok;
}

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static void wait_key(void)
{
    uint8_t b[16];
    plat_read(b, (int)sizeof b, -1);
    // A key that sends a sequence (an arrow) must not leave its tail behind.
    while (plat_read(b, (int)sizeof b, 60) > 0) {}
}

bool plat_run(const char *name, const char *args, const asm_result_t *r, char *msg, int msglen)
{
    uint32_t size;
    uint8_t *m = mod_build(name, r, &size);
    char path[48];
    snprintf(path, sizeof path, "/tmp/%s", name);
    bool ok = m && plat_save(path, (const char *)m, (long)size);
    free(m);
    if (!ok) {
        snprintf(msg, (size_t)msglen, "Could not write %s.", path);
        return false;
    }

    ubiqos_child_pulse(PULSE_CHILD);
    char argline[128];
    snprintf(argline, sizeof argline, "%s", args ? args : "");
    int32_t pid = ubiqos_exec(path, argline);
    if (pid < 0) {
        ubiqos_child_pulse(0);
        ubiqos_fs_remove(path);
        snprintf(msg, (size_t)msglen,
                 "%s would not start: is one still running, or is %s a command?", name, name);
        return false;
    }
    // The program owns the keyboard, Ctrl-C included, until it ends.
    ubiqos_foreground(UBIQOS_STDIN, pid);
    uint32_t reason = UBIQOS_END_EXITED;
    uint32_t started = ubiqos_ticks_now();
    for (;;) {
        ubiqos_msg_t mm;
        int32_t from = ubiqos_receive_tmo(&mm, UBIQOS_TIMEOUT_FOREVER);
        if (from > 0) { ubiqos_reply(-1); continue; }
        if (from == 0 && mm.type == PULSE_CHILD && UBIQOS_END_PID(mm.len) == pid) {
            reason = UBIQOS_END_REASON(mm.len);
            break;
        }
    }
    uint32_t ms = ubiqos_ticks_now() - started;
    int32_t self = ubiqos_pidof("blixt");
    ubiqos_foreground(UBIQOS_STDIN, self > 0 ? self : 0);
    ubiqos_child_pulse(0);
    ubiqos_fs_remove(path);

    const char *how = reason == UBIQOS_END_FAULTED ? "stopped by a fault"
                    : reason == UBIQOS_END_KILLED ? "was stopped (Ctrl-C or kill)"
                    : "ended";
    snprintf(msg, (size_t)msglen, "%s %s after %u ms.", name, how, (unsigned)ms);
    say("\x1b[0m\r\n");
    say(reason == UBIQOS_END_FAULTED ? "*** " : "--- ");
    say(msg);
    say(" Press a key to return to Blixt. ");
    wait_key();
    return reason == UBIQOS_END_EXITED;
}

const char *plat_name(void)
{
    static char b[64];
    char board[40] = "", ver[24] = "";
    int32_t n = ubiqos_config_get(UBIQOS_CFG_BOARD, board, sizeof board - 1);
    if (n > 0 && n < (int32_t)sizeof board) board[n] = 0; else board[0] = 0;
    n = ubiqos_config_get(UBIQOS_CFG_VERSION, ver, sizeof ver - 1);
    if (n > 0 && n < (int32_t)sizeof ver) ver[n] = 0; else ver[0] = 0;
    snprintf(b, sizeof b, "UbiqOS %s%s%s", ver, board[0] ? ", " : "", board);
    return b;
}

int blixt_main(int argc, char **argv);
int main(int argc, char **argv) { return blixt_main(argc, argv); }
