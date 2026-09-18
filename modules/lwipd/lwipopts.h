// lwIP configuration for UbiqOS.
//
// NO_SYS: there are no lwIP threads and no mailboxes. The stack runs inside one
// UbiqOS process which owns it entirely, calls sys_check_timeouts periodically,
// and answers clients by message. That is why the module is declared SINGLE --
// lwIP's tables are globals, and there is exactly one of it.
#ifndef UBIQOS_LWIPOPTS_H
#define UBIQOS_LWIPOPTS_H

#include <stdint.h>

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_TIMERS                 1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (16 * 1024)

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_ICMP                   1
#define LWIP_DHCP                   1
#define LWIP_RAW                    1

// Zeroconf. mDNS insists on IGMP for IPv4, because it answers on a multicast
// address, and on a source of randomness for the delay before it replies --
// several machines answering the same query in the same millisecond is the
// thing the delay exists to avoid. It also keeps per-interface state, hence
// the client data slot.
#define LWIP_IGMP                   1
#define LWIP_MDNS_RESPONDER         1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1
#define MEMP_NUM_UDP_PCB            8

uint32_t ubiqos_lwip_rand(void);
#define LWIP_RAND()                 ubiqos_lwip_rand()

// Diagnostics go nowhere for now: a module has no printf, and routing lwIP's
// through the write syscall would put network chatter in the middle of whatever
// the shell is printing. Turn it on deliberately when something needs looking at.
#define LWIP_DEBUG                  0
#define LWIP_STATS                  0

#endif
