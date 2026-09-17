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

So the DMA chain stops and HSTX runs dry.

**Found, 31 Aug 2026, and it was none of the obvious three.** The hardcoded
channels 8 to 11 do not collide -- video has 1, 2 and 3. What collides is
`spoop()`, a function that ran at the top of every `start_read`, reconfigured
DMA channel 3 by number and wrote a constant into `CH3_AL1_CTRL`, and never
started it. Leftover scaffolding, right down to the name; nothing in the driver
reads that channel afterwards.

Channel 3 is the one that writes `al3_read_addr_trig` and restarts the video
chain. Overwrite it and the chain runs once more and stops -- which is precisely
the `ch3: count 1, BUSY 0` above. The reading was already here; what was missing
was the single line that touches a channel this driver does not own.

Still in the file and still ugly, but cleared: the hardcoded channels 8 to 11,
the PIO1 reprogramming, and `gpio_set_mask(1)` / `gpio_clr_mask(1)` on GPIO 0
around every transfer.

This also reframes a day of it: "helt svart i displayen" after an SDIO attempt
was read as a locked-up board and answered with three blind reflashes and three
locked boards. The board may well have been alive the whole time, with only its
picture gone.

The `CARD_TRY_SDIO_AT_BOOT` scaffold is gone. Nothing mounts the card at
startup at all now, and SDIO is asked for by name with `mount sdio`.

## The waits, bounded

Upstream leaves seven waits open -- bare `while (...);` loops and ones marked
"todo not forever" -- and four of them `printf` on every pass, so a state
machine that never arrives floods the console as well as hanging.

That matters more here than upstream. These run in the filesystem server at
priority 22, above the shell at 16, so for ever means the shell never runs
again: no echo, no prompt, and a USB console that stays enumerated because the
USB task is at 30. The board looks dead over the serial port and is not.
Measured that way on 31 Aug 2026 -- PC in the USB task, crash log empty, video
DMA still walking the framebuffer.

They now go through `SD_WAIT_OR_RETURN_STUCK`, next to the `safe_*` helpers it
matches. The macro returns `SD_ERR_STUCK`, and is named so that it is obvious
that it does. `acquiesce_sm` was already bounded; its "todo not forever" was
stale and is gone.

**With this and the `spoop()` removal, four-bit SDIO works.** `mount sdio` on a
freshly inserted card: `use_sdio` 1, `bus_width` `bw_wide`, files listed, and
the video chain still walking the framebuffer through the whole thing.

## The three DMA buffers are allocated, not declared

7 Sep 2026. `crcs`, `ctrl_words` and `pio_cmd_buf` were static arrays; they are
pointers now, filled in by `sd_set_dma_buffers` before anything uses them.
Nothing else about them changed, and in a build that keeps this driver in the
kernel the pointers can simply be set to three static arrays.

The reason is that this driver is now compiled into a library module — see
`modules/sdlib` — and a module lives in PSRAM. PSRAM sits behind the XIP cache
on the QMI bus, so what DMA writes there is not reliably what the processor
reads back, and what the processor writes is not reliably what DMA reads.

For a caller's *data* buffer that is fixable by bouncing through SRAM, and the
driver above this one does exactly that. These three are not data. `ctrl_words`
is a chain of DMA control blocks that the DMA engine reads to program itself,
and `pio_cmd_buf` is read the same way; there is no bounce for memory the
hardware fetches on its own behalf. They have to *be* in SRAM, which means
being asked for rather than declared.

`sd_dma_buffer_words()` says how much, in words, and the sizes are the ones the
arrays had.

## The DMA wait is a second, not eight million spins

17 Sep 2026. `safe_dma_wait_for_finish` gave up after a spin count, which is a
time only on the machine it was chosen on. On the Waveshare 4.3B -- system clock
at 120 MHz, the driver running from the module pool -- it gave up on the very
first command while the transfer was still under way: the probe found the
command channel finished, count zero, a moment later. It now measures, with a
check of the clock every 4096 spins.

That was not why SDIO failed there, as it turned out -- the card had been left
in SPI mode by an earlier boot, and only cutting the power cleared it -- but a
wait that is too short for one card is a wrong answer waiting to happen.

## Any PIO block, and standard-capacity cards

17 Sep 2026, for the Waveshare board built with `MYRTOS_NATIVE_USB=host`, where
pio1 is the panel's and pio0 is free.

**The block.** `pio1` was named in the global, in every DMA request
(`DREQ_PIO1_RX0`, `DREQ_PIO1_TX0`) and in every pin function
(`GPIO_FUNC_PIO1`). `MYRTOS_SD_PIO_INDEX` in `sd_card.h` chooses it now, pio1
unless the build says otherwise. The replacements are constant expressions, so
a pio1 build asks for the same numbers it did. The block's GPIO window is still
the caller's business: `modules/sdlib` sets it from `MYRTOS_SD_PIO_GPIO_BASE`,
16 by default for the Fruit Jam's GP34-39, 0 on the Waveshare board's GP10-15.

**The card.** Upstream passes the block number straight to CMD17 and CMD24,
which is right for SDHC and SDXC only. A standard-capacity card is addressed by
byte, and the 4.3B's card is one. `sd_is_high_capacity()` reports the CCS bit
from the ACMD41 answer the init loop already waits on, and `modules/sdlib`
multiplies by 512 when it is clear -- as its SPI path already did. Verified on
that card: FAT32 mounted over four-bit SDIO, and a file written over SDIO was
there after a power cycle.

`modules/sdlib/printf.c` changed too: it collects a call's output and hands it
to `myrtos_print`, because character-at-a-time `myrtos_putc` does not reach the
kernel log, and the Waveshare board has nowhere else to read it.
