// Which server answers for which network stack.
//
// A socket call is a message, not a function call: the stack that serves it
// waits on hardware -- SPI to a coprocessor, or a USB transfer -- and waiting
// inside a trap stops the machine. So every stack is a server process, and
// choosing between them is choosing a pid.
//
// That is what makes more than one affordable. The socket number carries its
// stack in the high byte, so the dispatcher looks up a pid and strips the byte
// before passing the index on; a stack never learns that another exists.
#include <stdint.h>
#include "../common/ubiqos_abi.h"

int32_t ubiqos_wifi_server_pid(void);

static int32_t net_pid[UBIQOS_NET_STACKS];

// A stack says it is ready by registering. NINA is not in this table: it was
// here first and its pid is asked for directly, so that a build with no lwIP
// has nothing to arrange.
int32_t ubiqos_net_register(uint32_t stack, int32_t pid)
{
    if (stack >= UBIQOS_NET_STACKS || stack == UBIQOS_NET_NINA) return -1;
    net_pid[stack] = pid;
    return 0;
}

// -1 when nothing serves that stack, which is what a program asking for lwIP
// on a machine without it should get: a refusal, not a silent fallback onto a
// different network.
int32_t ubiqos_net_pid(uint32_t stack)
{
    if (stack == UBIQOS_NET_NINA) return ubiqos_wifi_server_pid();
    if (stack >= UBIQOS_NET_STACKS) return -1;
    return net_pid[stack] ? net_pid[stack] : -1;
}
