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
