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

## The four-pin binary info mask

`sd_init_4pins` declared its pins as `0xfu << PICO_SD_DAT0_PIN`. The data pins
start at **36** on this board and `0xfu` is a 32-bit unsigned, so the shift was
undefined and the mask came out wrong. `0xfull` now.

The macro is not at fault: on a chip with more than thirty-two pins
`bi_pin_mask_with_names` does cast to `uint64_t` -- but it casts the result, and
by then the shift has already happened. It matters only to what picotool prints
about the image, which is why it went unseen; it is undefined behaviour either
way, and the compiler had been saying so on every build.

## Cleared by reading, before the next session with a scope

The pin window was the first suspect and it is not the problem. `PICO_RP2350A`
is 0 in the board header, so `NUM_BANK0_GPIOS` is 48 and the SDK's
`PICO_PIO_USE_GPIO_BASE` evaluates to 1 -- which is what makes
`sm_config_set_in_pins` and its siblings take real pin numbers. They are given
`sd_dat_pin_base` = 36 with a count of four, `pio_set_gpio_base(pio1, 16)` runs
before `sd_init_4pins`, and DAT1-3 do get `GPIO_FUNC_PIO1` a few lines after
DAT0. The runtime masks are all 64 bits. There is nothing left to find there by
reading.

What is left is what a scope answers: whether GP36-39 actually move when the
data read is issued.

One thing worth fixing whatever the outcome: the driver **hardcodes DMA channels
8, 9, 10 and 11 and never claims them** (`sd_cmd_dma_channel` and friends).
Nothing collides today -- Pico-PIO-USB takes channel 0 by a hardcoded mask and
video claims three more dynamically, so the low channels are all that are in use
-- but nothing reserves 8 to 11 either, and the next driver that claims
dynamically will be handed them silently.

## It was the order, and the driver was never the problem

SDIO works. Four-bit wide bus, full clock. What was wrong was that it was asked
second.

A card latches into SPI mode the moment it is addressed that way and stays there
until the power is cut. `myrtos_fat_remount` called `myrtos_sd_init` -- which is
SPI -- and then asked for SDIO, so every attempt for two days was made to a card
that no longer spoke it. No amount of pin fixing could have helped.

Measured, on a card pulled and reinserted so it had never been spoken to:

    sdio_refused = 0        SPI never used this power cycle
    use_sdio     = 1
    bus_width    = 2        bw_wide, four bits
    dbg_padoe    = 0x000C0000   CLK and CMD driven, DAT0-3 released
    sm[2].clkdiv = 0x00010000   divisor 1, raised from 50 after init

And what it looked like while it was failing, from the same registers:

    dbg_padout toggling         the clock was running
    dbg_padoe  = 0x00F40000     CMD released, DAT0-3 held as outputs
    FSTAT RXEMPTY all four      nothing ever received
    DMA ch11 count = 2, frozen  the command response never came

The card was being clocked and asked correctly and said nothing at all, which is
what a card in SPI mode does. `dbg_padout` and `dbg_padoe` are how you take a
scope reading on pads that sit under the card holder.

**What is left.** Boot still runs `spi_init_card`, so SDIO is only reachable if
there is no card in the socket at boot -- put one in afterwards and run `mount`.
That is not a usable system. The card's bring-up belongs in the filesystem
server, where it is already safe to hang, so that SDIO gets first refusal on
every boot. Until then the driver's unbounded waits, which upstream marks "todo
not forever", must stay out of anything that runs before the scheduler: trying
it there cost a boot and needed the BOOTSEL button.

## Every __breakpoint() is gone

There were four, and removing one was worse than removing none: the driver gave
up on the DMA as it was written to, and then hit the next one instead. Each sits
immediately before an honest error return that never ran.

    105, 122  bounded FIFO waits, return SD_ERR_STUCK
    139       the DMA wait, returns SD_ERR_STUCK
    639       the CMD8 check pattern, returns -1

The last is the one that matters most: it fires when the card does not answer
CMD8 at all, which is exactly what a card already latched into SPI mode does. So
the failure that most needed to be survivable was the one that stopped the
machine. With a probe attached `__breakpoint()` is a gift; without one it is an
`ebreak`, and our trap handler answers that by spinning in `wfi` for ever.

The ACMD41 ready loop is bounded too, at a thousand asks. Upstream spins there
until the card answers, and this now runs in the filesystem server -- a process
at priority 22, above the shell. Spinning for ever there starves the very thing
you would use to look at the problem.

## The SDIO attempt kills the video

Bisected, not guessed. Moving the SDIO attempt into every boot turned the screen
black while the machine stayed perfectly alive -- serial answered, eight
processes, both shells, the console server draining its ring. Taking the attempt
back out of boot brought the picture back.

What the video hardware looked like while it was black:

    ch_data=1  ch_count=2  ch_addr=3     the three channels video claims
    ch1: count 0, BUSY 0, read 0x20067348    stopped mid-framebuffer
    ch2: count 0, BUSY 0
    ch3: count 1, BUSY 0
    HSTX CSR 0x50050203                      still enabled, and starving

So the DMA chain stops and HSTX runs dry. Candidates, all in this driver and all
things it does that nothing else does: it hardcodes DMA channels 8 to 11 by
number and never claims them, it reprograms PIO1, and it writes GPIO 0 directly
with `gpio_set_mask(1)` and `gpio_clr_mask(1)` -- a debugging leftover around
every transfer.

This also reframes a day of it: "helt svart i displayen" after an SDIO attempt
was read as a locked-up board and answered with three blind reflashes and three
locked boards. The board may well have been alive the whole time, with only its
picture gone.

`CARD_TRY_SDIO_AT_BOOT` in fsserver.c is 0 until this is understood.
