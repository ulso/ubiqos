# A 640x480 picture in PSRAM

`-DUBIQOS_VIDEO=psram` keeps a 640x480 framebuffer, a byte a pixel, in PSRAM
and copies it into the same ring of 48 SRAM scanlines the character
generator fills. The console draws pixels into it as it does into the SRAM
framebuffer of `UBIQOS_VIDEO=framebuffer`. The default is still `chargen`.

## How

- **Framebuffer**: 300 kB from the PSRAM pool at boot. Everything that draws
  writes through the no-cache alias (`ubiqos_framebuf`), because the copy
  reads the chip, not the cache.
- **Copy**: the pump, every 250 us, points the XIP streaming FIFO at the next
  run of lines and lets a DMA channel empty it into the ring. A run has to
  be contiguous in both the ring and the picture (which the console scrolls
  by moving its origin), so it stops at either edge and the next pump takes
  the rest; at 700 us that halved the slack and the beam caught up about 28
  lines a second.
- **Cost**: the pump takes about 4 us, so the display's share of the
  processor is about 0 (the character generator's is about 10 % at
  240 MHz). The C heap has 98 kB, against 48 kB with the character
  generator.

## Measured, 21 Nov 2026 (Fruit Jam, 240 MHz, PSRAM at 120)

- `psrambench lines` copied 5.7 million lines in this pattern with no wrong
  word, no WiFi link error and no lost ping.
- Idle and with heavy console output: no late scanlines.
- **Program loads and TLS starve it.** The XIP stream runs "in the
  background, using flash idle cycles": starting `agent` cost about 18000
  late scanlines, an https fetch about 16000, the boot about 21000. That is
  under a second of disturbed picture each time.
- Plain DMA from the no-cache alias instead fixed the picture (26 late lines
  for the same `agent` start) and broke the WiFi link: thousands of bad
  frame headers on the SPI to the C6. A DMA read that waits on PSRAM holds
  the DMA's read port, and the SPI receive FIFO overflows behind it. High
  priority on the SPI channels did not help -- a read already waiting is not
  rescheduled. So the stream it is.

## Not yet seen on a monitor

The one receiver to hand, an MS2130 capture dongle, showed this mode once
(with flicker, before the pump went to 250 us) and black on every later
boot, also with colour bars written into the framebuffer over the probe --
while the character generator, on the same build settings, always shows.
HSTX registers, clk_hstx, the HDMI pins' pads and functions, every GPIO,
the decoded frame table and the frame rate (55.144 Hz) are identical
between the two. The same dongle also failed to show 800x600 (see
video-800x600.md). A real monitor has to settle it.
