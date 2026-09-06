#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"
#include "probe.pio.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

// Does PIO work when the core is RISC-V?
//
// It should: the state machines are their own hardware and the core only writes
// registers at them. But the question was asked, and the way to answer it is to
// run one rather than to reason about it.
//
// The ARM port asked the same question a second time and got the same answer,
// which is the answer the reasoning gave -- so the message says which core it
// was actually running on rather than assuming.
void myrtos_pio_probe(void) {
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &probe_program);
    uint sm = 0;

    pio_sm_config c = probe_program_get_default_config(offset);
    sm_config_set_clkdiv(&c, 125.0f);          // slow; nothing here is in a hurry
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    myrtos_print("PIO: program at ");
    myrtos_print_u32(offset);
    myrtos_print(", waiting for the state machine... ");

    uint32_t spins = 0;
    while (pio_sm_is_rx_fifo_empty(pio, sm) && spins < 2000000) spins++;

    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        myrtos_print("nothing came back\n");
    } else {
        uint32_t v = pio_sm_get(pio, sm);
        myrtos_print("got ");
        myrtos_print_u32(v);
        if (v != 21) {
            myrtos_print(" -- wrong value\n");
        } else {
#ifdef __riscv
            myrtos_print(" -- PIO runs from RISC-V\n");
#else
            myrtos_print(" -- PIO runs from ARM\n");
#endif
        }
    }

    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &probe_program, offset);
}
