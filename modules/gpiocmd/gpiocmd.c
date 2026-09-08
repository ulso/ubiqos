#include "../../common/myrtos_abi.h"

// gpio -- the digital pins.
//
//   gpio                 every pin, its level and who owns it
//   gpio N in|up|down|out   set one up
//   gpio N 0|1           drive one this driver holds
//   gpio N free          give it back
//
// The listing is the useful part. This board fixes most of its pins in
// hardware and the drivers take several more, so "which pins can I actually
// use" is a real question with a short answer -- and asking for one that is
// taken says who has it rather than doing something surprising. GP44 is the
// terminal.

MYRTOS_MEM_SIZE(8192);

static uint32_t to_u32(const char *p, bool *ok)
{
    uint32_t v = 0, n = 0;
    for (; *p >= '0' && *p <= '9'; p++, n++) v = v * 10u + (uint32_t)(*p - '0');
    if (!n || *p) *ok = false;
    return v;
}

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: gpio [PIN in|up|down|out|free|0|1]\n"
            "       gpio watch PIN [DEBOUNCE_MS]\n\n"
            "With no argument, every pin with its level and its owner.\n"
            "A pin something else owns is refused, and says who has it.\n"
            "watch waits for edges and prints them; it blocks rather than polling.\n"))
        return;

    int32_t fd = myrtos_open("/dev/gpio");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "gpio: no /dev/gpio\n"); return; }

    myrtos_line_t l;

    // gpio watch PIN [MS] -- wait for edges and print them as they come.
    //
    // The read blocks. It does not poll: the driver's readable tells the
    // scheduler whether there is an event, the scheduler runs this process
    // again when there is, and in between it uses no time at all. ctrl-C ends
    // it, which is the shell killing this process and not something this
    // program has to arrange.
    if (argc > 2 && same(argv[1], "watch")) {
        bool k = true;
        myrtos_gpio_watch_t w = { .edges = MYRTOS_GPIO_FALL | MYRTOS_GPIO_RISE,
                                  .debounce_ms = 20 };
        w.pin = to_u32(argv[2], &k);
        if (argc > 3) w.debounce_ms = to_u32(argv[3], &k);
        if (!k || w.pin >= 48u) {
            myrtos_write_str(MYRTOS_STDERR, "usage: gpio watch PIN [DEBOUNCE_MS]\n");
            myrtos_close(fd); return;
        }
        if (myrtos_setstat(fd, MYRTOS_SS_GPIO_WATCH, &w, sizeof w) < 0) {
            myrtos_gpio_owner_t o = { .pin = w.pin };
            myrtos_getstat(fd, MYRTOS_SS_GPIO_OWNER, &o, sizeof o);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "gpio: GP");
            myrtos_line_u32(&l, w.pin);
            myrtos_line_str(&l, o.who[0] ? " belongs to " : " was refused");
            if (o.who[0]) myrtos_line_str(&l, o.who);
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDERR, &l);
            myrtos_close(fd); return;
        }
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "watching GP");
        myrtos_line_u32(&l, w.pin);
        myrtos_line_str(&l, ", ctrl-C to stop\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        for (;;) {
            myrtos_gpio_event_t e;
            int32_t n = myrtos_read(fd, &e, sizeof e);
            if (n != (int32_t)sizeof e) continue;
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "GP");
            myrtos_line_u32(&l, e.pin);
            myrtos_line_str(&l, e.level ? " released at " : " pressed at ");
            myrtos_line_u32(&l, e.at_ms);
            myrtos_line_str(&l, " ms\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
        }
    }

    if (argc > 1 && same(argv[1], "debug")) {
        uint32_t d[6] = {0};
        if (myrtos_getstat(fd, MYRTOS_SS_GPIO_DEBUG, d, sizeof d) < 0) {
            myrtos_write_str(MYRTOS_STDERR, "gpio: no debug\n"); myrtos_close(fd); return;
        }
        static const char *n[] = { "inte0", "intr0", "ints0", "calls", "pending", "dropped" };
        for (uint32_t i = 0; i < 6u; i++) {
            myrtos_line_reset(&l);
            myrtos_line_str(&l, n[i]); myrtos_line_str(&l, " ");
            myrtos_line_u32(&l, d[i]); myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
        }
        myrtos_close(fd); return;
    }

    if (argc < 2) {
        uint64_t all = 0;
        myrtos_getstat(fd, MYRTOS_SS_GPIO_LEVELS, &all, sizeof all);
        for (uint32_t p = 0; p < 48u; p++) {
            myrtos_gpio_owner_t o = { .pin = p };
            myrtos_getstat(fd, MYRTOS_SS_GPIO_OWNER, &o, sizeof o);
            myrtos_line_reset(&l);
            myrtos_line_str(&l, "GP");
            if (p < 10u) myrtos_line_str(&l, " ");
            myrtos_line_u32(&l, p);
            myrtos_line_str(&l, "  ");
            myrtos_line_str(&l, (all >> p) & 1u ? "1" : "0");
            myrtos_line_str(&l, "  ");
            myrtos_line_str(&l, o.who[0] ? o.who : "free");
            myrtos_line_str(&l, "\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
        }
        myrtos_close(fd);
        return;
    }

    bool ok = true;
    uint32_t pin = to_u32(argv[1], &ok);
    if (!ok || pin >= 48u || argc < 3) {
        myrtos_write_str(MYRTOS_STDERR, "usage: gpio [PIN in|up|down|out|free|0|1]\n");
        myrtos_close(fd);
        return;
    }

    const char *w = argv[2];
    myrtos_gpio_t g = { .pin = pin };
    uint32_t code = MYRTOS_SS_GPIO_MODE;

    if      (same(w, "in"))   g.value = MYRTOS_PIN_IN;
    else if (same(w, "up"))   g.value = MYRTOS_PIN_IN_PULLUP;
    else if (same(w, "down")) g.value = MYRTOS_PIN_IN_PULLDN;
    else if (same(w, "out"))  g.value = MYRTOS_PIN_OUT;
    else if (same(w, "free")) g.value = MYRTOS_PIN_RELEASE;
    else if (same(w, "0") || same(w, "1")) {
        code = MYRTOS_SS_GPIO_LEVEL;
        g.value = w[0] == '1';
    } else {
        myrtos_write_str(MYRTOS_STDERR, "gpio: in, up, down, out, free, 0 or 1\n");
        myrtos_close(fd);
        return;
    }

    if (myrtos_setstat(fd, code, &g, sizeof g) == 0) { myrtos_close(fd); return; }

    // Refused, and the interesting part is why. Almost always: something else
    // owns it.
    myrtos_gpio_owner_t o = { .pin = pin };
    myrtos_getstat(fd, MYRTOS_SS_GPIO_OWNER, &o, sizeof o);
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "gpio: GP");
    myrtos_line_u32(&l, pin);
    if (o.who[0]) {
        myrtos_line_str(&l, " belongs to ");
        myrtos_line_str(&l, o.who);
        myrtos_line_str(&l, "\n");
    } else if (code == MYRTOS_SS_GPIO_LEVEL) {
        myrtos_line_str(&l, " is not set up as an output yet\n");
    } else {
        myrtos_line_str(&l, " was refused\n");
    }
    myrtos_line_flush(MYRTOS_STDERR, &l);
    myrtos_close(fd);
}
