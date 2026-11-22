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

## On a monitor, 22 Nov 2026

A Raspberry Pi Monitor shows it sharp and stable. An https fetch (about
15000 late scanlines each, as above) shows as a short disturbance, under a
second, and the picture comes back by itself.

The MS2130 capture dongle tried first showed this mode once and black on
every later boot, while the character generator always showed; HSTX
registers, clocks, pads, the frame table and the frame rate were identical
between the two. That was the dongle, not the signal.

## Drawing on it from a program

`ubiqos_video_take_framebuffer(&fb)` (common/ubiqos_abi.h) gives a process
the picture: `fb.pixels`, `width`, `height`, `stride`, RGB332, a byte a
pixel, (0, 0) top left. While a process holds it the console draws nothing
on the monitor and drops what it is sent there; the USB console is not
affected. `ubiqos_video_give_framebuffer()` hands it back, and so does the
process ending, however it ends -- the console then starts on a clear
screen. On the character generator there are no pixels and the call says
no. `modules/fbdemo` is the worked example: a test card with a one-pixel
frame round the edge, and a bouncing square.

Measured 22 Nov 2026 on the Raspberry Pi Monitor: all of the card shows,
frame included. Drawing the whole card -- 300 kB of stores past the cache
-- takes 23 ms and costs about 4300 late scanlines, a short disturbance as
it appears; the square at 60 frames a second costs none. Large redraws are
where a program pays: they compete with the copy for the same QMI.
