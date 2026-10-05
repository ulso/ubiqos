// lwIP on the NUCLEO-H563ZI's Ethernet, and the thread that is lwIP's one
// context.
//
// On the RP2350 that context is the USB device task, which polls the cable's
// interface, the WiFi's, the clock and the socket server in one loop -- NO_SYS
// is 1 and lwIP has no locking, so everything that touches it has to be the
// same thread. This board has no USB task and one interface, so the loop is
// its own thread and does the same things: frames in, the link, lwIP's timers,
// the clock, and the socket calls of every process on the machine.
//
// The interface is en0: DHCP for its address, and mDNS so that it answers to
// its hostname.local -- the same name the Fruit Jam's interfaces answer to,
// from /sd/config.txt, which this board has no card for, so "ubiqos".

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/apps/mdns.h"
#include "netif/ethernet.h"
#include "config.h"
#include "usbdev.h"
#include "../../../common/ubiqos_abi.h"
#include "pico/time.h"
#include "port.h"
#include "eth.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
int32_t ubiqos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);

static struct netif en;
static bool started;

// The counters SYS_NETDEV reports, under the names the RP2350's lwipnet.c gives
// them, and the MAC it reports under the name TinyUSB's network class gives it
// there. What they count here is the Ethernet's.
uint32_t ubiqos_lwip_in, ubiqos_lwip_out, ubiqos_lwip_dropped;
uint8_t  tud_network_mac_address[6];

u32_t sys_now(void) { return (u32_t)(time_us_64() / 1000u); }

// LWIP_RAND is rand(), for DHCP's transaction ids and the ports a connection
// starts from. newlib's rand keeps its state in a structure it allocates, which
// drags in malloc, sbrk and a dozen system calls this kernel has no use for; the
// chip has a random number generator, so it is asked instead.
int rand(void)
{
    uint32_t v = 0;
    h5_rng_read((uint8_t *)&v, sizeof v);
    return (int)(v & 0x7FFFFFFFu);
}

// --- out and in -----------------------------------------------------------------

static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    static uint8_t flat[1536];
    if (p->tot_len > sizeof flat) return ERR_IF;
    const uint16_t len = pbuf_copy_partial(p, flat, p->tot_len, 0);
    if (!h5_eth_send(flat, len)) { ubiqos_lwip_dropped++; return ERR_IF; }
    ubiqos_lwip_out++;
    return ERR_OK;
}

static void deliver(const uint8_t *frame, uint32_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) { ubiqos_lwip_dropped++; return; }
    pbuf_take(p, frame, (u16_t)len);
    ubiqos_lwip_in++;
    if (en.input(p, &en) != ERR_OK) pbuf_free(p);
}

static err_t en_init(struct netif *n)
{
    n->name[0] = 'e';                     // 'en0' to anything that prints it
    n->name[1] = 'n';
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, tud_network_mac_address, 6);
    // All four, as on every UbiqOS interface: ETHERNET and ETHARP for ARP to
    // happen at all, IGMP so that the mDNS responder may join its group.
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
             | NETIF_FLAG_IGMP;
    n->output = etharp_output;
    n->linkoutput = link_output;
    return ERR_OK;
}

static void print_ip(const ip4_addr_t *a)
{
    const uint8_t *b = (const uint8_t *)&a->addr;
    for (int i = 0; i < 4; i++) { ubiqos_print_u32(b[i]); if (i < 3) ubiqos_print("."); }
}

static void on_status(struct netif *n)
{
    if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) return;
    ubiqos_print("net: address ");
    print_ip(netif_ip4_addr(n));
    ubiqos_print(" mask ");
    print_ip(netif_ip4_netmask(n));
    ubiqos_print(" router ");
    print_ip(netif_ip4_gw(n));
    ubiqos_print("\n");
#if LWIP_MDNS_RESPONDER
    mdns_resp_announce(n);
#endif
}

// A locally administered address, from the chip's own 96-bit id, so that two
// of these boards on one network do not answer to the same one.
static void make_mac(uint8_t mac[6])
{
    uint8_t id[12];
    h5_unique_id(id);
    mac[0] = 0x02;                        // local, unicast
    for (int i = 1; i < 6; i++) mac[i] = id[i - 1] ^ id[i + 5];
    mac[5] ^= id[11];
}

// --- the thread -------------------------------------------------------------------

#define LINK_POLL_MS 250u

static void net_thread(void)
{
    // The hostname is in the configuration, and the configuration is read by the
    // filesystem server, which may not have got there yet.
    while (!ubiqos_config_done()) ubiqos_sleep(10);

    make_mac(tud_network_mac_address);
    const char *why = h5_eth_start(tud_network_mac_address);
    if (why) {
        ubiqos_print("net: no Ethernet: ");
        ubiqos_print(why);
        ubiqos_print("\n");
        return;
    }

    lwip_init();
    netif_add(&en, NULL, NULL, NULL, NULL, en_init, ethernet_input);
    const char *host = ubiqos_config_hostname();
    netif_set_hostname(&en, host);
    netif_set_status_callback(&en, on_status);
    netif_set_default(&en);
    netif_set_up(&en);
#if LWIP_MDNS_RESPONDER
    mdns_resp_init();
    if (mdns_resp_add_netif(&en, host) == ERR_OK)
        mdns_resp_add_service(&en, host, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
#endif
    { extern void ubiqos_lwip_sock_init(void); ubiqos_lwip_sock_init(); }
    started = true;

    ubiqos_print("net: Ethernet up as ");
    ubiqos_print(host);
    ubiqos_print(", MAC ");
    for (int i = 0; i < 6; i++) {
        const char *hex = "0123456789abcdef";
        char b[3] = { hex[tud_network_mac_address[i] >> 4], hex[tud_network_mac_address[i] & 15], 0 };
        ubiqos_print(b);
        if (i < 5) ubiqos_print(":");
    }
    ubiqos_print("; waiting for a link\n");

    uint32_t next_link = 0;
    for (;;) {
        for (int budget = 0; budget < 8 && h5_eth_receive(deliver); budget++) { }

        const uint32_t now = sys_now();
        if ((int32_t)(now - next_link) >= 0) {
            next_link = now + LINK_POLL_MS;
            bool full = false;
            const uint32_t speed = h5_eth_link(&full);
            if (speed && !netif_is_link_up(&en)) {
                netif_set_link_up(&en);
                ubiqos_print("net: link up, ");
                ubiqos_print_u32(speed);
                ubiqos_print(full ? " Mbit/s full duplex; asking DHCP\n" : " Mbit/s half duplex; asking DHCP\n");
                dhcp_start(&en);
#if LWIP_MDNS_RESPONDER
                mdns_resp_announce(&en);
#endif
            } else if (!speed && netif_is_link_up(&en)) {
                netif_set_link_down(&en);
                dhcp_stop(&en);
                ubiqos_print("net: link down\n");
            }
        }

        sys_check_timeouts();
        { extern void ubiqos_sntp_poll(void); ubiqos_sntp_poll(); }
        { extern void ubiqos_lwip_serve(void); ubiqos_lwip_serve(); }
        ubiqos_sleep(1);
    }
}

void h5_net_start(void)
{
    if (ubiqos_kernel_thread(net_thread, 6144, UBIQOS_PRIO_WIFI) < 0)
        ubiqos_print("net: could not start its thread\n");
}

// --- what the rest of the kernel asks -------------------------------------------------

bool ubiqos_lwip_started(void) { return started; }

uint32_t ubiqos_lwip_addr(void)
{
    return started ? lwip_ntohl(netif_ip4_addr(&en)->addr) : 0u;
}

// The same thirty-one numbers lwipnet.c gives SYS_NETDEV on the RP2350, in the
// same places, so that the same `netstat`-like tools read them.
void ubiqos_lwip_stats(uint32_t *out)
{
    memset(out, 0, 31 * sizeof(uint32_t));
    out[0] = lwip_stats.link.recv;
    out[1] = lwip_stats.link.drop;
    out[2] = lwip_stats.link.err;
    out[3] = lwip_stats.etharp.recv;
    out[4] = lwip_stats.etharp.xmit;
    out[5] = lwip_stats.ip.recv;
    out[6] = lwip_stats.ip.drop;
    out[7] = lwip_stats.icmp.recv;
    out[8] = lwip_stats.icmp.xmit;
    out[9] = lwip_stats.ip.chkerr;
    extern uint32_t ubiqos_lwipsock_served, ubiqos_lwipsock_queued, ubiqos_lwipsock_taken,
                    ubiqos_lwipsock_recv, ubiqos_lwipsock_sent, ubiqos_lwipsock_why,
                    ubiqos_lwipsock_lastop, ubiqos_lwipsock_lastreply,
                    ubiqos_lwipsock_oncalls, ubiqos_lwipsock_onbytes;
    out[15] = ubiqos_lwipsock_served;
    out[16] = ubiqos_lwipsock_queued;
    out[17] = ubiqos_lwipsock_taken;
    out[18] = ubiqos_lwipsock_recv;
    out[19] = ubiqos_lwipsock_sent;
    out[20] = ubiqos_lwipsock_why;
    out[21] = ubiqos_lwipsock_lastop;
    out[22] = ubiqos_lwipsock_lastreply;
    out[23] = ubiqos_lwipsock_oncalls;
    out[24] = ubiqos_lwipsock_onbytes;
    out[25] = lwip_stats.tcp.recv;
    out[26] = lwip_stats.tcp.drop;
    out[27] = lwip_stats.tcp.err;
    out[28] = lwip_stats.tcp.chkerr;
    out[29] = lwip_stats.tcp.proterr;
    out[30] = lwip_stats.tcp.xmit;
}
