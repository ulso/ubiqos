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
            "usage: gpio [PIN in|up|down|out|free|0|1]\n\n"
            "With no argument, every pin with its level and its owner.\n"
            "A pin something else owns is refused, and says who has it.\n"))
        return;

    int32_t fd = myrtos_open("/dev/gpio");
    if (fd < 0) { myrtos_write_str(MYRTOS_STDERR, "gpio: no /dev/gpio\n"); return; }

    myrtos_line_t l;

    if (argc < 2) {
        uint8_t buf[8];
        int32_t n = myrtos_read(fd, buf, sizeof buf);
        uint64_t all = 0;
        for (int32_t i = 0; i < n; i++) all |= (uint64_t)buf[i] << (i * 8);
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
