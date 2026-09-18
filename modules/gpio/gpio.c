// Digital I/O, and the first driver here that has to ask before it acts.
//
// /dev/gpio hands out pins one at a time, and refuses any pin something else
// already owns. That refusal is the point. The pins a person would reach for
// on this board are the header ones -- D6 to D10 and A0 to A5 -- and four of
// those six analogue ones are the ADC and one of them, A4 on GP44, is the
// terminal. Without the registry, "gpio 44 out" would take the console away
// and the machine would go quiet with no explanation at all.
//
// A read gives every pin's input level, two 32-bit words with the low pins
// first, so a caller can look at everything at once. Setting a pin up and
// driving it are setstats, because they are commands and not data -- see
// UBIQOS_SS_GPIO_MODE beside them in the ABI.
//
// This driver takes no interrupt and holds no state the kernel cares about.
// It is the ordinary kind.

#include "../../common/ubiqos_abi.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/timer.h"
#include "hardware/regs/intctrl.h"

static const ubiqos_kernel_api_t *K;
static bool ready;

// Which pins this driver has taken, so that it can give them back and so that
// a second claim of the same pin is not refused by the registry.
static uint64_t mine;

// --- EDGES ----------------------------------------------------------------
// The handler records what happened and nothing else. It may not wake anybody
// -- see the rule beside irq_install -- and it does not need to: the scheduler
// already polls readable for a process blocked on a read, so a driver that
// answers readable honestly gets its reader woken for free. That is the whole
// mechanism, and none of it is in the handler.
//
// AT 0x80, NOT 0x40. The ADC's handler outranks the kernel because it has a
// hard deadline; a button does not. Sitting at the ordinary peripheral level
// means a kernel critical section does hold this off, which is exactly right
// -- a few microseconds late to a button press is not a thing anybody can
// measure, and the rules that come with outranking the kernel are not worth
// taking on for nothing.

#define EVENTS 32u

static volatile ubiqos_gpio_event_t ring[EVENTS];
static volatile uint32_t head, tail;      // head is the handler's, tail the reader's
static volatile uint32_t dropped;
static volatile uint32_t calls;
static uint32_t debounce_us[48];
static uint32_t last_us[48];
static bool irq_taken;

static void gpio_handler(void)
{
    calls++;
    uint32_t now = timer_hw->timerawl;
    for (uint32_t r = 0; r < 6u; r++) {
        uint32_t st = io_bank0_hw->proc0_irq_ctrl.ints[r];
        if (!st) continue;
        // Cleared before the events are dealt with, so an edge arriving during
        // this loop is not thrown away with the ones being handled.
        io_bank0_hw->intr[r] = st;
        for (uint32_t b = 0; b < 8u; b++) {
            uint32_t pin = r * 8u + b;
            if (pin >= 48u) break;
            uint32_t bits = (st >> (b * 4u)) & 0xcu;    // the two edge bits
            if (!bits) continue;
            if (debounce_us[pin] && (now - last_us[pin]) < debounce_us[pin]) continue;
            last_us[pin] = now;
            uint32_t next = (head + 1u) % EVENTS;
            if (next == tail) { dropped++; continue; }   // reader is behind
            ring[head].pin = (uint8_t)pin;
            ring[head].level = (uint8_t)((bits & 0x8u) ? 1u : 0u);   // rise or fall
            ring[head].reserved = 0;
            ring[head].at_ms = now / 1000u;
            head = next;
        }
    }
}

static int32_t gpio_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;
    ready = true;
    return 0;
}

static int32_t gpio_open(void) { return ready ? 0 : -1; }

// Edge events, oldest first. The pin levels are a getstat instead: a level is
// a fact about now and a stream is what a reader waits on, and mixing the two
// into one read would mean guessing which the caller wanted.
static int32_t gpio_read(uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;
    uint32_t n = 0;
    while (len - n >= sizeof(ubiqos_gpio_event_t) && tail != head) {
        ubiqos_gpio_event_t e = ring[tail];
        tail = (tail + 1u) % EVENTS;
        for (uint32_t i = 0; i < sizeof e; i++) buf[n + i] = ((const uint8_t *)&e)[i];
        n += sizeof e;
    }
    return (int32_t)n;
}

// What makes a blocked reader run again. The scheduler polls this; the handler
// never touches the scheduler.
static int32_t gpio_readable(void)
{
    if (!ready) return 0;
    uint32_t pending = (head - tail) % EVENTS;
    return (int32_t)(pending * sizeof(ubiqos_gpio_event_t));
}

// What the pins are called on this board, which is not the same question as
// who owns them. The buttons own nothing and are free to read; A0 to A5 and D6
// to D10 are just header positions. Printing the name beside "free" is the
// difference between a list of numbers and a list somebody can use.
static const char *board_label(uint32_t pin)
{
    switch (pin) {
    case 0:  return "boot/button1";
    case 4:  return "button2";
    case 5:  return "button3";
    case 6:  return "D6";
    case 7:  return "D7";
    case 8:  return "D8";
    case 9:  return "D9";
    case 10: return "D10";
    case 40: return "A0";
    case 41: return "A1";
    case 42: return "A2";
    case 43: return "A3";
    case 44: return "A4";
    case 45: return "A5";
    default: return 0;
    }
}

static int32_t gpio_getstat(uint32_t code, void *data, uint32_t len)
{
    if (code == UBIQOS_SS_GPIO_DEBUG) {
        if (!data || len != 24u) return -1;
        uint32_t *o = (uint32_t *)data;
        o[0] = io_bank0_hw->proc0_irq_ctrl.inte[0];
        o[1] = io_bank0_hw->intr[0];
        o[2] = io_bank0_hw->proc0_irq_ctrl.ints[0];
        o[3] = calls;
        o[4] = (head - tail) % EVENTS;
        o[5] = dropped;
        return 0;
    }
    if (code == UBIQOS_SS_GPIO_LEVELS) {
        if (!data || len != 8u) return -1;
        uint64_t all = 0;
        for (uint32_t p = 0; p < 48u; p++)
            if (K->gpio_get(p)) all |= 1ull << p;
        *(uint64_t *)data = all;
        return 0;
    }
    if (code != UBIQOS_SS_GPIO_OWNER || !data || len != sizeof(ubiqos_gpio_owner_t))
        return -1;
    ubiqos_gpio_owner_t *o = (ubiqos_gpio_owner_t *)data;
    if (o->pin >= 48u) return -1;
    // The owner if there is one, otherwise what the board calls the pin, in
    // brackets so a reader can tell the two apart at a glance.
    const char *w = K->pin_owner(o->pin);
    uint32_t i = 0;
    if (w) {
        for (; w[i] && i < sizeof o->who - 1u; i++) o->who[i] = w[i];
    } else if ((w = board_label(o->pin)) != 0) {
        o->who[i++] = '(';
        for (uint32_t j = 0; w[j] && i < sizeof o->who - 2u; j++) o->who[i++] = w[j];
        o->who[i++] = ')';
    }
    o->who[i] = 0;
    return 0;
}

static int32_t gpio_watch(const ubiqos_gpio_watch_t *w)
{
    if (w->pin >= 48u) return -1;
    uint64_t bit = 1ull << w->pin;
    uint32_t reg = w->pin / 8u, shift = (w->pin % 8u) * 4u;
    // EDGE_LOW is bit 2 of the pin's nibble and EDGE_HIGH bit 3, which is why
    // the handler tests 0x8 to tell a rise from a fall.
    uint32_t mask = ((w->edges & UBIQOS_GPIO_FALL) ? 4u : 0u)
                  | ((w->edges & UBIQOS_GPIO_RISE) ? 8u : 0u);

    if (!w->edges) {
        io_bank0_hw->proc0_irq_ctrl.inte[reg] &= ~(0xcu << shift);
        debounce_us[w->pin] = 0;
        return 0;
    }

    // The pin has to be ours before it is watched, exactly as it does before
    // it is driven. A button on GP0 is also the BOOT button, which the board
    // table already claims -- so watching it is refused, and that is correct
    // rather than unfortunate.
    if (!(mine & bit)) {
        if (K->pin_claim(w->pin, "gpio") < 0) return -1;
        mine |= bit;
        K->gpio_init(w->pin);
    }
    K->gpio_set_dir(w->pin, false);

    if (!irq_taken) {
        // 0x80, the ordinary peripheral level. See the note above the handler.
        if (K->irq_install(IO_IRQ_BANK0, gpio_handler, 0x80u) < 0) return -1;
        irq_taken = true;
    }
    debounce_us[w->pin] = w->debounce_ms * 1000u;
    last_us[w->pin] = 0;
    io_bank0_hw->intr[reg] = 0xcu << shift;             // forget any edge already latched
    io_bank0_hw->proc0_irq_ctrl.inte[reg] =
        (io_bank0_hw->proc0_irq_ctrl.inte[reg] & ~(0xcu << shift)) | (mask << shift);
    return 0;
}

static int32_t gpio_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (!ready || !data) return -1;

    if (code == UBIQOS_SS_GPIO_WATCH) {
        if (len != sizeof(ubiqos_gpio_watch_t)) return -1;
        return gpio_watch((const ubiqos_gpio_watch_t *)data);
    }

    if (len != sizeof(ubiqos_gpio_t)) return -1;
    const ubiqos_gpio_t *g = (const ubiqos_gpio_t *)data;
    if (g->pin >= 48u) return -1;
    uint64_t bit = 1ull << g->pin;

    if (code == UBIQOS_SS_GPIO_MODE) {
        if (g->value == UBIQOS_PIN_RELEASE) {
            if (!(mine & bit)) return -1;
            ubiqos_gpio_watch_t off = { g->pin, 0, 0 };
            gpio_watch(&off);
            K->gpio_set_dir(g->pin, false);
            K->gpio_set_pulls(g->pin, false, false);
            K->pin_release(g->pin);
            mine &= ~bit;
            return 0;
        }
        // Claimed before it is touched, not after. A pin that belongs to the
        // terminal or the ADC is refused here, which is the whole reason this
        // driver cannot simply do what it is told.
        if (!(mine & bit)) {
            if (K->pin_claim(g->pin, "gpio") < 0) return -1;
            mine |= bit;
            K->gpio_init(g->pin);
        }
        switch (g->value) {
        case UBIQOS_PIN_IN:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, false, false); return 0;
        case UBIQOS_PIN_IN_PULLUP:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, true, false); return 0;
        case UBIQOS_PIN_IN_PULLDN:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, false, true); return 0;
        case UBIQOS_PIN_OUT:
            K->gpio_set_dir(g->pin, true); return 0;
        default:
            return -1;
        }
    }

    if (code == UBIQOS_SS_GPIO_LEVEL) {
        // Only a pin this driver holds. Driving one it does not own would be
        // exactly the accident the registry exists to prevent.
        if (!(mine & bit)) return -1;
        K->gpio_put(g->pin, g->value != 0u);
        return 0;
    }
    return -1;
}

static bool gpio_init_mod(const ubiqos_kernel_api_t *api)
{
    if (!api || api->abi != UBIQOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const ubiqos_driver_module_t ubiqos_driver = {
    .abi = UBIQOS_DRIVER_ABI,
    .reserved = 0,
    .init = gpio_init_mod,
    .ops = {
        .module_name = "gpiodev",
        .configure = gpio_configure,
        .open = gpio_open, .read = gpio_read, .write = 0,
        .readable = gpio_readable,
        .getstat = gpio_getstat, .setstat = gpio_setstat,
    },
};
