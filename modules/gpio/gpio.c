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
// MYRTOS_SS_GPIO_MODE beside them in the ABI.
//
// This driver takes no interrupt and holds no state the kernel cares about.
// It is the ordinary kind.

#include "../../common/myrtos_abi.h"

static const myrtos_kernel_api_t *K;
static bool ready;

// Which pins this driver has taken, so that it can give them back and so that
// a second claim of the same pin is not refused by the registry.
static uint64_t mine;

static int32_t gpio_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;
    ready = true;
    return 0;
}

static int32_t gpio_open(void) { return ready ? 0 : -1; }

static int32_t gpio_read(uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;
    uint32_t want = len < 8u ? len : 8u;
    uint64_t all = 0;
    for (uint32_t p = 0; p < 48u; p++)
        if (K->gpio_get(p)) all |= 1ull << p;
    for (uint32_t i = 0; i < want; i++) buf[i] = (uint8_t)(all >> (i * 8u));
    return (int32_t)want;
}

static int32_t gpio_getstat(uint32_t code, void *data, uint32_t len)
{
    if (code != MYRTOS_SS_GPIO_OWNER || !data || len != sizeof(myrtos_gpio_owner_t))
        return -1;
    myrtos_gpio_owner_t *o = (myrtos_gpio_owner_t *)data;
    if (o->pin >= 48u) return -1;
    const char *w = K->pin_owner(o->pin);
    uint32_t i = 0;
    if (w) for (; w[i] && i < sizeof o->who - 1u; i++) o->who[i] = w[i];
    o->who[i] = 0;
    return 0;
}

static int32_t gpio_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (!ready || !data || len != sizeof(myrtos_gpio_t)) return -1;
    const myrtos_gpio_t *g = (const myrtos_gpio_t *)data;
    if (g->pin >= 48u) return -1;
    uint64_t bit = 1ull << g->pin;

    if (code == MYRTOS_SS_GPIO_MODE) {
        if (g->value == MYRTOS_PIN_RELEASE) {
            if (!(mine & bit)) return -1;
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
        case MYRTOS_PIN_IN:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, false, false); return 0;
        case MYRTOS_PIN_IN_PULLUP:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, true, false); return 0;
        case MYRTOS_PIN_IN_PULLDN:
            K->gpio_set_dir(g->pin, false); K->gpio_set_pulls(g->pin, false, true); return 0;
        case MYRTOS_PIN_OUT:
            K->gpio_set_dir(g->pin, true); return 0;
        default:
            return -1;
        }
    }

    if (code == MYRTOS_SS_GPIO_LEVEL) {
        // Only a pin this driver holds. Driving one it does not own would be
        // exactly the accident the registry exists to prevent.
        if (!(mine & bit)) return -1;
        K->gpio_put(g->pin, g->value != 0u);
        return 0;
    }
    return -1;
}

static bool gpio_init_mod(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = gpio_init_mod,
    .ops = {
        .module_name = "gpiodev",
        .configure = gpio_configure,
        .open = gpio_open, .read = gpio_read, .write = 0,
        .getstat = gpio_getstat, .setstat = gpio_setstat,
    },
};
