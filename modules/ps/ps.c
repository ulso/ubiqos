#include "../../common/myrtos_abi.h"

// ps -- what is running, and what each process is waiting for.
//
// The state is the useful column. A process that is stuck is stuck on something
// specific -- input, a child, or a length of time -- and until this existed
// there was no way to see which, or to see that anything was stuck at all.

// One string with the names end to end, rather than a switch returning literals.
// That switch compiled to a table of their addresses, and a table of addresses
// is exactly what a position-independent module may not contain -- the build's
// check refused the module, which is what it is for.
// The order is the ABI's constant order, not a readable one: FREE, READY,
// RUNNING, WAIT_READ, WAIT_CHILD, SLEEPING, WAIT_WRITE, WAIT_RECV, WAIT_REPLY.
// WAIT_WRITE came later and was given 6 so the earlier numbers would not move,
// which is why it sits out of sequence here. Getting it wrong is silent -- the
// first version had run and ready swapped and simply reported the wrong thing,
// and for a while a process waiting to write showed up as a question mark.
static const char STATE_NAMES[] = "?\0ready\0run\0read\0child\0sleep\0write\0recv\0reply";

static const char *state_name(uint32_t s) {
    const char *p = STATE_NAMES;
    if (s > MYRTOS_PS_WAIT_REPLY) return p;
    while (s--) { while (*p) p++; p++; }
    return p;
}

static void pad(myrtos_line_t *l, uint32_t written, uint32_t width) {
    while (written++ < width) myrtos_line_str(l, " ");
}

static uint32_t digits(uint32_t v) {
    uint32_t n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

void module_main(void) {
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "\n pid  pri  state  memory  name\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    for (uint32_t slot = 0; slot < MYRTOS_PS_SLOTS; slot++) {
        myrtos_psinfo_t p;
        if (myrtos_psinfo(slot, &p) < 0) continue;

        myrtos_line_reset(&l);
        myrtos_line_str(&l, " ");
        myrtos_line_u32(&l, p.pid);
        pad(&l, digits(p.pid) + 1, 5);
        myrtos_line_u32(&l, p.priority);
        pad(&l, digits(p.priority), 5);

        const char *st = state_name(p.state);
        myrtos_line_str(&l, st);
        uint32_t n = 0;
        while (st[n]) n++;
        pad(&l, n, 7);

        myrtos_line_u32(&l, p.mem_size);
        pad(&l, digits(p.mem_size), 8);

        myrtos_line_str(&l, p.name);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }
}
