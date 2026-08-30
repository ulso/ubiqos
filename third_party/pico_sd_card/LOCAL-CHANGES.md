# Local changes to pico_sd_card

Vendored from pico-extras at 52fd7a7. One change, in `sd_card.c`:

**Pin masks widened to 64 bits.** The driver comes from the RP2040 world, where
every GPIO fits in a 32-bit mask. This board has the card on GP34 to GP39, and
`0xf << 36` in a `uint32_t` is taken modulo 32 on RISC-V -- it became `0xf << 4`,
so the pin directions were set for GPIO 2 to 7 and nothing drove the card. The
SDK says as much in `pio.h`: the 32-bit form "only works for pins < 32".
`pio_sm_set_pindirs_with_mask64` is the one to use.

The caller must also set the PIO's GPIO base, since one PIO instance reaches
either GPIO 0-31 or 16-47 and the default is the low window. That is done in
`kernel/sdcard.c` rather than here, because it is a property of where the board
puts the card rather than of the driver.

The `sm_config_` calls needed nothing: with `PICO_PIO_USE_GPIO_BASE` they take
real pin numbers in the full range.

## Where it actually stops

Found with a J-Link Ultra over SWD, device `RP2350_RV32_0`, which halts the
Hazard3 core and gives a backtrace against `os_kernel.elf`. It is not a hang:

    mcause 0x3  (breakpoint)
    mepc   -> safe_dma_wait_for_finish, sd_card.c:133
              called from finish_read, sd_card.c:312

`safe_dma_wait_for_finish` waits for a DMA channel, gives up after a limit,
prints "stuck dma channel" and executes `__breakpoint()`. So the driver gets as
far as issuing a **data read** -- the command phase appears to work -- and then
no data ever arrives on DAT0-3 and the DMA never finishes.

That narrows the next question a great deal: it is the data lines, not the
command line, and not the pin masks that were fixed above. Worth checking next:
whether the DAT state machine's `in` pins are configured for the right window,
whether the four data pins really are driven (a scope on GP36-39 would say), and
whether DMA channels 8-11, which this driver hardcodes rather than claiming,
collide with anything by the time SD init runs.
