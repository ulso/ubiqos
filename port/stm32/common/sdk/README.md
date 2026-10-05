# The pico-sdk calls the kernel makes, answered for the STM32

The kernel was written on the RP2350 against the Raspberry Pi pico-sdk, and a
handful of its files include SDK headers for a handful of functions: the time
in microseconds, interrupts off and on, a busy wait, a section the startup code
does not clear. Those headers are here under the SDK's own names, holding only
what the kernel uses, so that the kernel's sources need not change for this
chip and the RP2350 build stays byte for byte what it was.

Nothing here is the SDK or derived from it. Each function is a few lines over
the Cortex-M's own registers or over this port; see each family's sdk.c and
timer.c.

What is RP2350 hardware rather than SDK -- the TRNG, the ring oscillator, the
PSRAM, POWMAN's reset reason -- is not faked here. The kernel asks the port for
those under UBIQOS_CHIP_STM32, and port.h says what the port offers.
