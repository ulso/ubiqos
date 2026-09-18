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
// Both, because there are two networks now and they want different answers.
//
// The WiFi network has a real router, and asking it is the only way to get an
// address anybody else can route to. The USB link has a fixed address and the
// board is the DHCP server there -- kernel/lwipdhcpd.c, which is ours and not
// lwIP's, so nothing below turns it on.
//
// DHCP costs 6.7 kB of SRAM on arm and 8.4 on riscv, measured, and 68 bytes of
// that is its variables -- the rest is dhcp.c's code, which this kernel keeps
// in SRAM because it is linked copy_to_ram. The heap figure moves by ±4 kB
// either side of that on alignment alone, so it is the wrong number to read.
#define LWIP_DHCP                   1
// Off since 18 Sep 2026: the cable has its own subnet now, and 169.254 on a
// computer with more than one network was routed out of the wrong one.
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    1

// A name ending in .local is asked for by MULTICAST rather than of a DNS
// server, which is what makes `ping fruit-jam.local` work from the board the
// same way it works from the Mac. lwIP's own responder answers such questions;
// this is the other half, asking them.
#define LWIP_DNS_SUPPORT_MDNS_QUERIES 1
#define LWIP_NETIF_HOSTNAME         1

// --- mDNS AND DNS-SD -------------------------------------------------------
// So the board answers to a name instead of to a link-local address that moves
// whenever the MAC does -- AutoIP seeds from the hardware address, and flipping
// one bit of it moved this board from .92.150 to .91.150 in an afternoon.
//
// IGMP is not optional here and mdns.c says so with an #error: the responder
// joins 224.0.0.251, and IPv4 multicast needs group management. LWIP_RAND is
// required too, for the query jitter that keeps two responders from answering
// in the same millisecond.
#define LWIP_MDNS_RESPONDER         1
#define LWIP_IGMP                   1
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define MDNS_MAX_SERVICES           2
#define LWIP_RAND()                 ((u32_t)rand())


#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
// Eight thousand, not four. The four was tuned for ONE interface with no DHCP
// client on it. There are two now, each with an mDNS registration, and a DHCP
// client on one of them -- and the symptom of running out is not an error
// anywhere: httpd sent its headers, promised 4003 bytes in Content-Length, and
// then sent nothing at all while the connection stayed open. A stack that
// cannot allocate a segment simply stops, and the client waits.
#define MEM_SIZE                    8000
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_UDP_PCB            7    // a responder on each interface, and the cable's DHCP server
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            8
// Twenty-four, and it was twelve.
//
// Twelve was chosen when there was one interface. Counting what actually wants
// one now: TCP 1, ARP 1, DHCP 2, AutoIP 1, IGMP 1, DNS 1 -- that is seven
// before any application -- and then the mDNS responder, which takes several
// per interface while it probes and announces, and there are two interfaces.
//
// Running out is not a dropped timer. lwIP asserts, and an assert here is a
// panic: the board stopped dead with the display still running, USB gone and
// the keyboard with it, three times before the crash record was made to
// survive a reboot and could be read. The panic's own string named it exactly
// -- "pool MEMP_NUM_SYS_TIMEOUT is empty" -- which is what that record is for.
//
// Each slot is a dozen bytes. Being generous costs nothing worth counting.
#define MEMP_NUM_SYS_TIMEOUT        24
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

// The SNTP client is ours -- see kernel/lwipsntp.c. lwIP's own is a fine
// client and cost 4 kB of SRAM, which is 4 kB the RISC-V chargen build has not
// got. Nothing to configure here as a result.

#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0
#define LWIP_DEBUG                  0

#endif
