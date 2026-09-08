// The analogue inputs, and an interrupt that belongs to a driver rather than
// to the kernel.
//
// /dev/adc gives the last conversion of each channel as 16-bit little-endian
// values. It is also the first thing in this system with a handler of its own,
// which is what the BASEPRI work in kernel/critical.h was written for and had
// nothing to exercise it.
//
// THE HANDLER RUNS ABOVE THE KERNEL. At priority 0x40 against a threshold of
// 0x80 it is never masked by a critical section, which means it can and does
// run while the kernel is halfway through its own data structures. So it
// touches none of them: no system call, no message, no allocation, nothing
// that reaches the scheduler or the I/O manager. It reads the ADC's FIFO,
// writes two arrays this file owns, and stops. Everything a reader wants is
// taken from those arrays by the read path, in ordinary thread context.
//
// It keeps its state in plain statics rather than __thread. A driver is
// instantiated once, by the kernel, so there is no per-process copy to want --
// and in interrupt context the thread pointer belongs to whatever was
// interrupted, which makes thread-local storage exactly the wrong place.

#include "../../common/myrtos_abi.h"
#include "hardware/adc.h"
#include "hardware/resets.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/structs/iobank0.h"

static const myrtos_kernel_api_t *K;

// A0 to A3 on this board, which are ADC channels 0 to 3 on GP40 to GP43.
#define ADC_FIRST_PIN  40u
#define ADC_CHANNELS   4u
#define SAMPLE_HZ      8000u

// 0x40 against kernel/critical.h's 0x80: more urgent, so never masked. The
// whole point of the exercise, and the reason the number is written here in
// the same form the threshold uses.
#define ADC_PRIORITY   0x40u

// --- WHAT THE HANDLER OWNS ------------------------------------------------
// Written only by the handler, read by anybody. Each is a single 32-bit or
// 16-bit store, which is atomic on both machines, so a reader sees an old
// value or a new one and never half of each. That is the whole of the
// synchronisation and it is why there is no lock here to take.

static volatile uint16_t latest[ADC_CHANNELS];
static volatile uint32_t taken;         // conversions handled
static volatile uint32_t overruns;      // times the FIFO had already filled
static volatile uint32_t last_us;
static volatile uint32_t worst_gap_us;  // the number this was all built for
static volatile uint32_t best_gap_us = 0xffffffffu;
static uint32_t next_channel;

static void adc_handler(void)
{
    // The clock first and the work second: what is being measured is when the
    // handler ran, not how long it took.
    uint32_t now = timer_hw->timerawl;
    uint32_t prev = last_us;
    last_us = now;
    if (prev) {
        uint32_t gap = now - prev;
        if (gap > worst_gap_us) worst_gap_us = gap;
        if (gap < best_gap_us) best_gap_us = gap;
    }

    // Everything waiting, not just one. A handler that took a single sample
    // would turn one late run into a permanent backlog.
    uint32_t n = 0;
    while (!adc_fifo_is_empty()) {
        uint16_t v = adc_fifo_get();
        // Bit 15 is the error flag when err_in_fifo is on; it is not data.
        latest[next_channel] = (uint16_t)(v & 0x0fffu);
        next_channel = (next_channel + 1u) % ADC_CHANNELS;
        taken++;
        n++;
    }
    if (n > 1u) overruns++;
}

// --- SETTING IT UP ---------------------------------------------------------
// By register rather than through the SDK's adc_init and adc_set_clkdiv. A
// module links no SDK library code, only what the headers inline -- and
// adc_set_clkdiv takes a float, which on the RISC-V half of this system is a
// call to a soft-float routine that is not linked either. See modules/neopixel
// for the first time that caught somebody out.

static bool ready;

// NOTHING HAPPENS HERE. Not the pads, not the ADC block, not the interrupt.
//
// The first version did all of it at boot, and the machine went down before
// USB came up -- twice, each costing a BOOTSEL button and a person to press
// it. A driver whose bring-up can take the machine with it should be able to
// be brought up a step at a time from a shell, so that a failure is a power
// cycle and the step that caused it is known.
//
// MYRTOS_SS_RUN carries how far to go: 1 pads, 2 the ADC block and its clock,
// 3 the interrupt installed, 4 converting. Each step includes the ones before
// it. 0 stops.
static int32_t adc_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;
    ready = true;
    return 0;
}

static uint32_t stage;

// Returns false rather than warning. A K->print from inside a system call does
// not reach the console -- the message is queued behind a USB task that cannot
// run until the trap returns, and it was simply lost -- so the driver would
// have gone on to configure a pad it does not own, silently, which is the one
// thing the registry exists to stop. Refusing sends the failure back up to the
// caller, which does have somewhere to print.
static bool adc_pads(void)
{
    // What adc_gpio_init would do, by register. It is inline in the SDK's
    // header but calls gpio_set_input_enabled, which is not -- and a module
    // links no SDK library code. Hi-Z output, no pulls, and the digital
    // receiver off, which is what puts an analogue voltage on the pin instead
    // of a logic level.
    for (uint32_t i = 0; i < ADC_CHANNELS; i++) {
        uint32_t pin = ADC_FIRST_PIN + i;
        if (K->pin_claim(pin, "adc") < 0) return false;
        hw_write_masked(&padsbank0_hw->io[pin],
                        PADS_BANK0_GPIO0_OD_BITS,
                        PADS_BANK0_GPIO0_OD_BITS | PADS_BANK0_GPIO0_IE_BITS |
                        PADS_BANK0_GPIO0_PUE_BITS | PADS_BANK0_GPIO0_PDE_BITS);
        iobank0_hw->io[pin].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    }
    return true;
}

static void adc_block(void)
{
    reset_block_num(RESET_ADC);
    unreset_block_num_wait_blocking(RESET_ADC);
    hw_set_bits(&adc_hw->cs, ADC_CS_EN_BITS);
    while (!(adc_hw->cs & ADC_CS_READY_BITS)) { }

    adc_set_round_robin((1u << ADC_CHANNELS) - 1u);
    adc_select_input(0);
    // Threshold one: an interrupt per conversion, which is the point -- a
    // deeper threshold would hide exactly the latency being measured.
    adc_fifo_setup(true, false, 1, true, false);
    // 48 MHz over the rate, straight to the register. adc_set_clkdiv takes a
    // float, and a module has no soft-float to fall back on.
    adc_hw->div = ((48000000u / SAMPLE_HZ) - 1u) << ADC_DIV_INT_LSB;
}

static bool irq_taken;

static int32_t adc_to_stage(uint32_t want)
{
    if (want > 4u) return -1;
    if (want == 0u) {
        if (stage >= 4u) { adc_run(false); adc_irq_set_enabled(false); adc_fifo_drain(); }
        stage = 0;
        return 0;
    }
    if (want >= 1u && stage < 1u) { if (!adc_pads()) return -1; stage = 1; }
    if (want >= 2u && stage < 2u) { adc_block(); stage = 2; }
    if (want >= 3u && stage < 3u) {
        // Installed once and never given back: the SDK has no way to take an
        // exclusive handler off again that does not risk the vector being
        // wrong while an interrupt is in flight.
        if (!irq_taken) {
            if (K->irq_install(ADC_IRQ_FIFO, adc_handler, ADC_PRIORITY) < 0) return -1;
            irq_taken = true;
        }
        stage = 3;
    }
    if (want >= 4u && stage < 4u) {
        adc_fifo_drain();
        last_us = 0;
        adc_irq_set_enabled(true);
        adc_run(true);
        stage = 4;
    }
    return 0;
}

static int32_t adc_open(void) { return ready ? 0 : -1; }

// Named for the device it serves, not for the chip: hardware/adc.h already has
// an adc_read of its own that reads one conversion.
static int32_t adcdev_read(uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;
    uint32_t want = len / 2u;
    if (want > ADC_CHANNELS) want = ADC_CHANNELS;
    for (uint32_t i = 0; i < want; i++) {
        uint16_t v = latest[i];
        buf[i * 2] = (uint8_t)(v & 0xffu);
        buf[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    return (int32_t)(want * 2u);
}

static int32_t adc_readable(void) { return ready ? (int32_t)(ADC_CHANNELS * 2u) : 0; }

static int32_t adc_getstat(uint32_t code, void *data, uint32_t len)
{
    if (!data) return -1;
    if (code == MYRTOS_SS_RATE && len == 4) {
        *(uint32_t *)data = SAMPLE_HZ / ADC_CHANNELS;   // per channel
        return 0;
    }
    if (code == MYRTOS_SS_IRQSTATS && len == sizeof(myrtos_irqstats_t)) {
        myrtos_irqstats_t *o = (myrtos_irqstats_t *)data;
        o->taken = taken;
        o->overruns = overruns;
        o->worst_gap_us = worst_gap_us;
        o->best_gap_us = best_gap_us == 0xffffffffu ? 0u : best_gap_us;
        o->expected_us = 1000000u / SAMPLE_HZ;
        o->priority = stage;
        return 0;
    }
    return -1;
}

// Zeroing the worst case is a write, so it belongs here rather than in a read.
// Without it a single stall early on stands for ever and every later
// measurement is that one number.
static int32_t adc_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (code == MYRTOS_SS_RUN) {
        if (!data || len != 4) return -1;
        return adc_to_stage(*(const uint32_t *)data);
    }
    (void)data; (void)len;
    if (code != MYRTOS_SS_IRQSTATS) return -1;
    worst_gap_us = 0;
    best_gap_us = 0xffffffffu;
    overruns = 0;
    return 0;
}

static bool adc_init_mod(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = adc_init_mod,
    .ops = {
        .module_name = "adcdev",
        .configure = adc_configure,
        .open = adc_open, .read = adcdev_read, .write = 0,
        .readable = adc_readable,
        .getstat = adc_getstat, .setstat = adc_setstat,
    },
};
