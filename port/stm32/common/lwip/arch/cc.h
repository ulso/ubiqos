// lwIP's view of this compiler and this machine, for the STM32 port. The
// RP2350 build takes the pico-sdk's; this says the same things without it.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#if NO_SYS
typedef int sys_prot_t;
#endif

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

void ubiqos_print(const char *s);
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { ubiqos_print("lwIP assert: "); ubiqos_print(x); ubiqos_print("\n"); } while (0)
