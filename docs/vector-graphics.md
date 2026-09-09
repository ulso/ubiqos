# Vector graphics — work in progress

Written 9 September 2026, at the point where the work was put down. Everything
below was measured on hardware unless it says otherwise.

## What works

A scanline renderer in `kernel/vector.c`: segments sorted by their top end,
walked once per frame, with an active list whose x values advance by dx/dy.
Newman and Sproull's algorithm, used for the reason it was used then — there is
nowhere to keep a picture, only time to build one line at a time.

It composes **over** the character generator rather than replacing it, so a
program can put a label beside a plot without either knowing about the other.
`vec line`, `vec clear`, `vec demo`, `vec text on|off`.

`scope [channel] [sweeps] [rate]` draws `/dev/adc` over a graticule with a real
timebase: the ADC handler at priority 0x40 fills the sweep itself, and the span
is measured first sample to last rather than derived from the rate. Verified at
40 / 10 / 4 ms per division against 2000 / 8000 / 20000 samples per second, and
measured 39.9 / 9.9 / 3.9.

## What it costs

    text console alone                49 % of a 120 MHz Cortex-M33
    background only (vec text off)    23 %
    28 full-height diagonals         +19 points
    a 653-segment scope trace        + 9 points

Two limits, and conflating them produced a wrong answer once already:

- `MYRTOS_VEC_SEGMENTS` (1024) bounds **memory**, about 12 kB with the index.
- `MYRTOS_VEC_ACTIVE` (256) bounds **time**, per scanline.

## What is not finished

**The display's cost is the real blocker, and it is not a graphics problem.**
The character generator takes about half the processor, and that is what breaks
USB: the same ADC sweep at 32 kHz survives with `vec text off` (display at 19
per cent) and kills the USB device with the text on (51 per cent). Controlled
A/B, and the first deliberate reproduction of a fault that had been chased under
three wrong headings.

The direction worth trying is to render **a whole glyph row at a time** rather
than a scanline at a time. Per cell, the load, the attribute comparison and the
colour words are currently redone for each of the sixteen scanlines the glyph
occupies; done once per row they would be amortised sixteen ways. The ring holds
48 buffers, so sixteen at a time fits. Unmeasured.

**The per-scanline cap cannot tell the two dangerous shapes apart.** A
horizontal segment lives on exactly one scanline, so five hundred of them make
one expensive line that the ring's 1.5 ms absorbs. A full-height diagonal is
active on every line, and a hundred of those took the processor the keyboard
needed. One number cannot express that; what says which one you have is
vidstat's share of the CPU.

**`myrtos_vector_dropped` is built but never verified on hardware.** It counts
segments the cap refused, and vidstat reports it, so a truncated picture should
say so instead of looking like a signal that stops half way across. The board
lost its USB device before this was tried.

**The ADC channel map is wrong.** The board labels its inputs A1 to A5; the
driver covers GP40 to GP43 and calls them A0 to A3. GP40 reads raw zero always
and is not on the header — a ghost channel — while the board's A4 and A5 are not
covered at all. GP44 is the UART, so the obvious shift is not obviously right
either. Check the schematic before changing `ADC_FIRST_PIN`.

**The scope has no trigger and no vertical scale.** It free-runs, so a periodic
signal drifts across the screen, and the vertical axis is fixed at 0 to 3.3 V.
