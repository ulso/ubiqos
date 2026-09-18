#include "../../common/ubiqos_abi.h"

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

UBIQOS_MEM_SIZE(8192);

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
    if (ubiqos_help(argc, argv,
            "usage: gpio [PIN in|up|down|out|free|0|1]\n"
            "       gpio watch PIN [DEBOUNCE_MS]\n\n"
            "With no argument, every pin with its level and its owner.\n"
            "A pin something else owns is refused, and says who has it.\n"
            "watch waits for edges and prints them; it blocks rather than polling.\n"))
        return;

    int32_t fd = ubiqos_open("/dev/gpio");
    if (fd < 0) { ubiqos_write_str(UBIQOS_STDERR, "gpio: no /dev/gpio\n"); return; }

    ubiqos_line_t l;

    // gpio watch PIN [MS] -- wait for edges and print them as they come.
    //
    // The read blocks. It does not poll: the driver's readable tells the
    // scheduler whether there is an event, the scheduler runs this process
    // again when there is, and in between it uses no time at all. ctrl-C ends
    // it, which is the shell killing this process and not something this
    // program has to arrange.
    if (argc > 2 && same(argv[1], "watch")) {
        bool k = true;
        ubiqos_gpio_watch_t w = { .edges = UBIQOS_GPIO_FALL | UBIQOS_GPIO_RISE,
                                  .debounce_ms = 20 };
        w.pin = to_u32(argv[2], &k);
        if (argc > 3) w.debounce_ms = to_u32(argv[3], &k);
        if (!k || w.pin >= 48u) {
            ubiqos_write_str(UBIQOS_STDERR, "usage: gpio watch PIN [DEBOUNCE_MS]\n");
            ubiqos_close(fd); return;
        }
        if (ubiqos_setstat(fd, UBIQOS_SS_GPIO_WATCH, &w, sizeof w) < 0) {
            ubiqos_gpio_owner_t o = { .pin = w.pin };
            ubiqos_getstat(fd, UBIQOS_SS_GPIO_OWNER, &o, sizeof o);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "gpio: GP");
            ubiqos_line_u32(&l, w.pin);
            ubiqos_line_str(&l, o.who[0] ? " belongs to " : " was refused");
            if (o.who[0]) ubiqos_line_str(&l, o.who);
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDERR, &l);
            ubiqos_close(fd); return;
        }
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "watching GP");
        ubiqos_line_u32(&l, w.pin);
        ubiqos_line_str(&l, ", ctrl-C to stop\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        for (;;) {
            ubiqos_gpio_event_t e;
            int32_t n = ubiqos_read(fd, &e, sizeof e);
            if (n != (int32_t)sizeof e) continue;
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "GP");
            ubiqos_line_u32(&l, e.pin);
            ubiqos_line_str(&l, e.level ? " released at " : " pressed at ");
            ubiqos_line_u32(&l, e.at_ms);
            ubiqos_line_str(&l, " ms\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
        }
    }

    if (argc > 1 && same(argv[1], "debug")) {
        uint32_t d[6] = {0};
        if (ubiqos_getstat(fd, UBIQOS_SS_GPIO_DEBUG, d, sizeof d) < 0) {
            ubiqos_write_str(UBIQOS_STDERR, "gpio: no debug\n"); ubiqos_close(fd); return;
        }
        static const char *n[] = { "inte0", "intr0", "ints0", "calls", "pending", "dropped" };
        for (uint32_t i = 0; i < 6u; i++) {
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, n[i]); ubiqos_line_str(&l, " ");
            ubiqos_line_u32(&l, d[i]); ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
        }
        ubiqos_close(fd); return;
    }

    if (argc < 2) {
        uint64_t all = 0;
        ubiqos_getstat(fd, UBIQOS_SS_GPIO_LEVELS, &all, sizeof all);
        for (uint32_t p = 0; p < 48u; p++) {
            ubiqos_gpio_owner_t o = { .pin = p };
            ubiqos_getstat(fd, UBIQOS_SS_GPIO_OWNER, &o, sizeof o);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "GP");
            if (p < 10u) ubiqos_line_str(&l, " ");
            ubiqos_line_u32(&l, p);
            ubiqos_line_str(&l, "  ");
            ubiqos_line_str(&l, (all >> p) & 1u ? "1" : "0");
            ubiqos_line_str(&l, "  ");
            ubiqos_line_str(&l, o.who[0] ? o.who : "free");
            ubiqos_line_str(&l, "\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
        }
        ubiqos_close(fd);
        return;
    }

    bool ok = true;
    uint32_t pin = to_u32(argv[1], &ok);
    if (!ok || pin >= 48u || argc < 3) {
        ubiqos_write_str(UBIQOS_STDERR, "usage: gpio [PIN in|up|down|out|free|0|1]\n");
        ubiqos_close(fd);
        return;
    }

    const char *w = argv[2];
    ubiqos_gpio_t g = { .pin = pin };
    uint32_t code = UBIQOS_SS_GPIO_MODE;

    if      (same(w, "in"))   g.value = UBIQOS_PIN_IN;
    else if (same(w, "up"))   g.value = UBIQOS_PIN_IN_PULLUP;
    else if (same(w, "down")) g.value = UBIQOS_PIN_IN_PULLDN;
    else if (same(w, "out"))  g.value = UBIQOS_PIN_OUT;
    else if (same(w, "free")) g.value = UBIQOS_PIN_RELEASE;
    else if (same(w, "0") || same(w, "1")) {
        code = UBIQOS_SS_GPIO_LEVEL;
        g.value = w[0] == '1';
    } else {
        ubiqos_write_str(UBIQOS_STDERR, "gpio: in, up, down, out, free, 0 or 1\n");
        ubiqos_close(fd);
        return;
    }

    if (ubiqos_setstat(fd, code, &g, sizeof g) == 0) { ubiqos_close(fd); return; }

    // Refused, and the interesting part is why. Almost always: something else
    // owns it.
    ubiqos_gpio_owner_t o = { .pin = pin };
    ubiqos_getstat(fd, UBIQOS_SS_GPIO_OWNER, &o, sizeof o);
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "gpio: GP");
    ubiqos_line_u32(&l, pin);
    if (o.who[0]) {
        ubiqos_line_str(&l, " belongs to ");
        ubiqos_line_str(&l, o.who);
        ubiqos_line_str(&l, "\n");
    } else if (code == UBIQOS_SS_GPIO_LEVEL) {
        ubiqos_line_str(&l, " is not set up as an output yet\n");
    } else {
        ubiqos_line_str(&l, " was refused\n");
    }
    ubiqos_line_flush(UBIQOS_STDERR, &l);
    ubiqos_close(fd);
}
