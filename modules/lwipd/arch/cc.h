// The compiler and platform layer lwIP asks its port for.
#ifndef UBIQOS_LWIP_CC_H
#define UBIQOS_LWIP_CC_H

#include <stdint.h>
#include <stddef.h>

#define LWIP_NO_UNISTD_H     1
#define LWIP_TIMEVAL_PRIVATE 1

// Declared by sys.h whether or not lightweight protection is on, so it needs a
// type even when nothing calls it. There is one process in the stack and no
// interrupt reaches into it, so there is nothing to protect against.
typedef int sys_prot_t;

void ubiqos_lwip_assert(const char *why);

#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { ubiqos_lwip_assert(x); } while (0)

#endif
