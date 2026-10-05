// What lwIP asks of the platform, on every STM32 board with a network: the
// time in milliseconds, and random numbers. On the RP2350 the pico-sdk's
// pico_lwip_nosys gives both.

#include <stdint.h>
#include <stdlib.h>
#include "lwip/arch.h"
#include "pico/time.h"
#include "port.h"

u32_t sys_now(void) { return (u32_t)(time_us_64() / 1000u); }

// LWIP_RAND is rand(), for DHCP's transaction ids and the ports a connection
// starts from. newlib's rand keeps its state in a structure it allocates, which
// drags in malloc, sbrk and a dozen system calls this kernel has no use for; the
// chip has a random number generator, so it is asked instead.
int rand(void)
{
    uint32_t v = 0;
    stm32_rng_read((uint8_t *)&v, sizeof v);
    return (int)(v & 0x7FFFFFFFu);
}
