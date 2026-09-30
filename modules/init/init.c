#include "../../common/ubiqos_abi.h"

// init -- the first process on a node, and the only one that may start others.
//
// The kernel starts it and nothing else. It reads its table, the data module
// inittab, and starts each program there in the table's order: at the
// priority the table gives, and only once the program the table says to wait
// for is serving -- blocked in receive, not merely started, since started is
// not the same as ready to answer. Then it stays, as the node's supervisor:
// the kernel sends it a pulse whenever one of its children ends, saying which
// and why, and the table says whether that program is started again.
//
//   # program   priority  restart  after     arguments
//   nodelog     20        always   -
//   nodebeat    16        always   nodelog
//
// restart is always, fault (only when a fault or a stack overflow ended it) or
// never. A program restarted five times within a minute is given up on and
// said so, rather than restarted for ever at a second's interval.
//
// It serves nobody: a message sent to it is answered -1.
UBIQOS_MEM_SIZE(2048);

#define MAX_ENTRIES     8           // a node has eight process slots
#define PULSE_CHILD     0x6e69u     // "in"; any type would do, it is ours
#define READY_WAIT_MS   5000u       // for the program an entry waits for
#define RESTART_DELAY   1000u
#define RESTART_LIMIT   5u          // within RESTART_WINDOW
#define RESTART_WINDOW  60000u

enum { RESTART_NEVER, RESTART_FAULT, RESTART_ALWAYS };

typedef struct {
    char     name[UBIQOS_NAME_LEN];
    char     after[UBIQOS_NAME_LEN];
    const char *args;               // into the table, which stays linked
    uint32_t args_len;
    uint32_t priority;
    uint32_t restart;
    int32_t  pid;                   // -1 while not running
    uint32_t due;                   // tick to restart at, when restart_due
    bool     restart_due;
    bool     given_up;
    uint32_t window_start, restarts;
} entry_t;

static entry_t entries[MAX_ENTRIES];
static uint32_t count;

static void log_line(ubiqos_line_t *l) {
    ubiqos_line_str(l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, l);
}

// --- the table ---------------------------------------------------------------

static bool is_space(char c) { return c == ' ' || c == '\t'; }

// One word into out, at most cap-1 characters; returns where the next begins.
static const char *word(const char *p, const char *end, char *out, uint32_t cap) {
    while (p < end && is_space(*p)) p++;
    uint32_t n = 0;
    while (p < end && !is_space(*p) && *p != '\n') {
        if (n < cap - 1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

static uint32_t number(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return v;
}

static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void read_table(const char *t, uint32_t size) {
    const char *end = t + size;
    while (t < end && *t && count < MAX_ENTRIES) {
        const char *eol = t;
        while (eol < end && *eol && *eol != '\n') eol++;

        const char *p = t;
        while (p < eol && is_space(*p)) p++;
        if (p < eol && *p != '#') {
            entry_t *e = &entries[count];
            char prio[8], restart[8];
            p = word(p, eol, e->name, sizeof e->name);
            p = word(p, eol, prio, sizeof prio);
            p = word(p, eol, restart, sizeof restart);
            p = word(p, eol, e->after, sizeof e->after);
            while (p < eol && is_space(*p)) p++;
            e->args = p;
            e->args_len = (uint32_t)(eol - p);
            e->priority = number(prio);
            if (e->priority < 1 || e->priority > UBIQOS_PRIO_MAX) e->priority = UBIQOS_PRIO_DEFAULT;
            e->restart = same(restart, "always") ? RESTART_ALWAYS
                       : same(restart, "fault")  ? RESTART_FAULT : RESTART_NEVER;
            if (same(e->after, "-")) e->after[0] = 0;
            e->pid = -1;
            count++;
        }
        t = eol + 1;
    }
}

// --- starting ------------------------------------------------------------------

// Whether NAME is running and blocked in receive: serving, not just started.
static bool serving(const char *name) {
    const int32_t pid = ubiqos_pidof(name);
    if (pid < 0) return false;
    ubiqos_psinfo_t info;
    return ubiqos_psinfo((uint32_t)pid, &info) == 0 && info.state == UBIQOS_PS_WAIT_RECV;
}

static void start(entry_t *e) {
    ubiqos_line_t l;
    if (e->after[0]) {
        uint32_t waited = 0;
        while (!serving(e->after) && waited < READY_WAIT_MS) { ubiqos_sleep(10); waited += 10; }
        if (waited >= READY_WAIT_MS) {
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "init: ");
            ubiqos_line_str(&l, e->after);
            ubiqos_line_str(&l, " is not serving; starting ");
            ubiqos_line_str(&l, e->name);
            ubiqos_line_str(&l, " anyway");
            log_line(&l);
        }
    }

    // A child takes its parent's priority, so init takes the child's for the
    // moment it starts it, and goes back to its own.
    char args[64];
    uint32_t n = e->args_len < sizeof args - 1 ? e->args_len : sizeof args - 1;
    for (uint32_t i = 0; i < n; i++) args[i] = e->args[i];
    args[n] = 0;
    const int32_t own = ubiqos_setprio(e->priority);
    e->pid = ubiqos_exec(e->name, args);
    ubiqos_setprio((uint32_t)own);

    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "init: ");
    ubiqos_line_str(&l, e->name);
    if (e->pid < 0) {
        ubiqos_line_str(&l, " did not start: ");
        ubiqos_line_str(&l, e->pid == UBIQOS_EXEC_NO_MEMORY ? "no memory"
                          : e->pid == UBIQOS_EXEC_NO_SLOT   ? "no process slot"
                          : e->pid == UBIQOS_EXEC_NOT_FOUND ? "not in flash" : "refused");
        e->pid = -1;
    } else {
        ubiqos_line_str(&l, " started, pid ");
        ubiqos_line_u32(&l, (uint32_t)e->pid);
        ubiqos_line_str(&l, ", priority ");
        ubiqos_line_u32(&l, e->priority);
    }
    log_line(&l);
}

// --- supervising ---------------------------------------------------------------

static void ended(int32_t pid, uint32_t reason) {
    entry_t *e = 0;
    for (uint32_t i = 0; i < count; i++) if (entries[i].pid == pid) e = &entries[i];
    if (!e) return;                             // not one of ours
    e->pid = -1;

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "init: ");
    ubiqos_line_str(&l, e->name);
    ubiqos_line_str(&l, reason == UBIQOS_END_FAULTED ? " faulted"
                      : reason == UBIQOS_END_KILLED  ? " was killed" : " exited");

    const bool again = e->restart == RESTART_ALWAYS
                    || (e->restart == RESTART_FAULT && reason == UBIQOS_END_FAULTED);
    const uint32_t now = ubiqos_ticks_now();
    if (again && now - e->window_start > RESTART_WINDOW) { e->window_start = now; e->restarts = 0; }
    if (!again) {
        ubiqos_line_str(&l, "; not restarted");
    } else if (e->restarts >= RESTART_LIMIT) {
        ubiqos_line_str(&l, "; restarted ");
        ubiqos_line_u32(&l, RESTART_LIMIT);
        ubiqos_line_str(&l, " times within a minute, giving up");
        e->given_up = true;
    } else {
        e->restarts++;
        e->restart_due = true;
        e->due = now + RESTART_DELAY;
        ubiqos_line_str(&l, "; restarting in a second");
    }
    log_line(&l);
}

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_line_t l;

    uint32_t size = 0;
    const char *table = ubiqos_data_link("inittab", &size);
    if (!table) {
        ubiqos_write_str(UBIQOS_STDOUT, "init: no inittab; nothing to start\n");
        return;
    }
    read_table(table, size);          // kept linked: the entries point into it

    ubiqos_child_pulse(PULSE_CHILD);  // before the first child, so none is missed

    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "init: ");
    ubiqos_line_u32(&l, count);
    ubiqos_line_str(&l, " programs in inittab");
    log_line(&l);
    for (uint32_t i = 0; i < count; i++) start(&entries[i]);

    for (;;) {
        // Wait for a child to end, or for the next restart to fall due.
        uint32_t wait = UBIQOS_TIMEOUT_FOREVER;
        const uint32_t now = ubiqos_ticks_now();
        for (uint32_t i = 0; i < count; i++) {
            if (!entries[i].restart_due) continue;
            const int32_t left = (int32_t)(entries[i].due - now);
            const uint32_t w = left > 0 ? (uint32_t)left : 0u;
            if (w < wait) wait = w;
        }

        ubiqos_msg_t m;
        const int32_t from = ubiqos_receive_tmo(&m, wait);
        if (from == 0 && m.type == PULSE_CHILD) {
            ended(UBIQOS_END_PID(m.len), UBIQOS_END_REASON(m.len));
        } else if (from > 0) {
            ubiqos_reply(-1);             // init serves nobody
        }

        const uint32_t t = ubiqos_ticks_now();
        for (uint32_t i = 0; i < count; i++) {
            entry_t *e = &entries[i];
            if (e->restart_due && (int32_t)(t - e->due) >= 0) {
                e->restart_due = false;
                start(e);
            }
        }
    }
}
