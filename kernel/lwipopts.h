// lwIP, configured for one context and no operating system underneath it.
//
// NO_SYS = 1 means lwIP has no threads, no mutexes and no reentrancy: every
// call into it must come from the SAME place. That place is the USB device
// task, because tud_network_recv_cb is called from there and a frame can then
// go straight into the netif with no queue between -- and TinyUSB's own flow
// control does the rest, since returning false there means "ask me again".
#ifndef MYRTOS_LWIPOPTS_H
#define MYRTOS_LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0   // the sequential APIs need threads
#define LWIP_NETCONN                0
#define SYS_LIGHTWEIGHT_PROT        0

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   0   // AutoIP instead; see below
#define LWIP_AUTOIP                 1
#define LWIP_DNS                    1
#define LWIP_NETIF_HOSTNAME         1

// AutoIP and not DHCP, and that is the whole address story for a link with one
// host on the other end. macOS gave itself 169.254.221.131 the moment the
// interface appeared, with nothing offering DHCP -- so a server would have
// been answering a question nobody asked. The seed address is lwIP's own,
// which is the MAC's last two bytes: mine took several statements where the
// macro is used as an expression, and had nothing to add.

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            8
#define MEMP_NUM_SYS_TIMEOUT        8
#define PBUF_POOL_SIZE              6
#define PBUF_POOL_BUFSIZE           1536

#define TCP_MSS                     1460
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            8

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// Checksums in software. The RP2350 has no offload and the frames are small.
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1

#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define LWIP_DEBUG                  0

#endif
