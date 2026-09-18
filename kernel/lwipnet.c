// lwIP on the USB network device.
//
// One context and only one: NO_SYS = 1 means lwIP has no locking of its own,
// so everything here is called from the USB device task and from nowhere else.
// A frame arrives in tud_network_recv_cb, which runs there, and goes straight
// into the netif -- no queue, because TinyUSB already has one: returning false
// from the callback means "ask me again", which is the flow control.
//
// The address is fixed -- 192.168.7.1, or usb_address in /sd/config.txt -- and
// the computer at the other end is given the next one by the DHCP server in
// lwipdhcpd.c. It was AutoIP, on the reasoning that the host takes a 169.254
// address anyway and a server would answer a question nobody asked. The host
// did take one, and then routed 169.254 through some other interface of its
// own: see the note at the top of lwipdhcpd.c.
#include <stdint.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "lwip/apps/mdns.h"
#include "config.h"
#include "class/net/net_device.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

static struct netif nif;
static bool started;

uint32_t myrtos_lwip_in, myrtos_lwip_out, myrtos_lwip_dropped;

// What kind of frames arrive, counted before lwIP sees them. This separates
// "the frame never came" from "the stack did not take it", which is the whole
// question when ARP counts zero and IP counts four.
uint32_t myrtos_lwip_arp_frames, myrtos_lwip_ip4_frames, myrtos_lwip_other_frames;

// IPv4 frames actually addressed to us, and of those, how many are ICMP. This
// is the last thing the counters cannot already answer: whether the echo
// request arrives at all, or arrives and is refused.
uint32_t myrtos_lwip_for_us, myrtos_lwip_icmp_frames;

uint32_t myrtos_lwip_addr(void);

// --- OUT ------------------------------------------------------------------
// tud_network_xmit hands the pbuf back to tud_network_xmit_cb, which copies it
// into the driver's buffer. The pbuf has to stay alive until then, and it does:
// this call does not return before the copy, so there is nothing to keep.
static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (!tud_network_can_xmit(p->tot_len))
        return ERR_IF;                    // no room; lwIP will retry
    tud_network_xmit(p, 0);
    myrtos_lwip_out++;
    return ERR_OK;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    if (!p) return 0;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

// --- IN -------------------------------------------------------------------
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (!started || !size) { tud_network_recv_renew(); return true; }

    // PBUF_POOL, so the payload is a copy this owns: the driver's buffer is
    // handed back the moment this returns.
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) {
        myrtos_lwip_dropped++;
        return false;                     // ask again -- do not lose it quietly
    }
    pbuf_take(p, src, size);
    myrtos_lwip_in++;

    if (size >= 14) {
        uint16_t type = (uint16_t)((src[12] << 8) | src[13]);
        if (type == 0x0806)      myrtos_lwip_arp_frames++;
        else if (type == 0x0800) {
            myrtos_lwip_ip4_frames++;
            if (size >= 34) {
                uint32_t dst = ((uint32_t)src[30] << 24) | ((uint32_t)src[31] << 16) |
                               ((uint32_t)src[32] << 8) | (uint32_t)src[33];
                if (dst == myrtos_lwip_addr()) {
                    myrtos_lwip_for_us++;
                    if (src[23] == 1) myrtos_lwip_icmp_frames++;   // protocol ICMP
                }
            }
        }
        else                     myrtos_lwip_other_frames++;
    }

    if (nif.input(p, &nif) != ERR_OK)
        pbuf_free(p);

    tud_network_recv_renew();
    return true;
}

void tud_network_init_cb(void) { }

// --- THE INTERFACE --------------------------------------------------------
static err_t if_init(struct netif *n)
{
    n->name[0] = 'u';                     // 'usb0' to anything that prints it
    n->name[1] = 's';
    n->mtu = 1500;
    n->hwaddr_len = 6;
    // ONE BIT DIFFERENT FROM THE HOST'S. In NCM the address in the descriptor
    // is what the HOST takes for its own end -- macOS showed en27 with exactly
    // the MAC this board advertises -- so giving the netif the same one puts
    // the same address at both ends of a two-node link. ARP then never
    // completes: the host asks, the reply is addressed to the asker's own MAC,
    // and its cache stays "(incomplete)" for ever.
    //
    // TinyUSB's own lwIP example flips this bit for the same reason.
    memcpy(n->hwaddr, tud_network_mac_address, 6);
    n->hwaddr[5] ^= 0x01;
    // NETIF_FLAG_ETHERNET as well as ETHARP: the first says the device speaks
    // ethernet at all, the second that it resolves addresses with ARP, and
    // leaving the first out is a link that answers nothing while the frames
    // arrive perfectly well.
    // NETIF_FLAG_IGMP as well, because the mDNS responder joins 224.0.0.251
    // and igmp_joingroup_netif refuses an interface that does not claim to do
    // group management -- mdns_resp_add_netif then fails with nothing said
    // about why. This is the second flag on this line to be found by its
    // absence; lwIP's own ethernet netif sets all of them together.
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
             | NETIF_FLAG_IGMP;
    n->output = etharp_output;
    n->linkoutput = link_output;
    return ERR_OK;
}

// What a browser or `dns-sd -B _http._tcp` sees beside the name. It is a
// callback rather than a table because the responder asks again on every
// announcement, so a value that changes does not need re-registering.
#if LWIP_MDNS_RESPONDER
static void http_txt(struct mdns_service *service, void *arg)
{
    (void)arg;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}
#endif

static void on_status(struct netif *n)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(n))) {
        const uint8_t *a = (const uint8_t *)&netif_ip4_addr(n)->addr;
        // Say it again with an address, because that is when a name becomes
        // useful to anybody -- and mdns_resp_announce is how the responder is
        // told the settings changed.
#if LWIP_MDNS_RESPONDER
        mdns_resp_announce(n);
#endif
        myrtos_print("net: address ");
        for (int i = 0; i < 4; i++) {
            myrtos_print_u32(a[i]);
            myrtos_print(i < 3 ? "." : "\n");
        }
    }
}

void myrtos_lwip_start(void)
{
    if (started) return;

    lwip_init();
    // A /24 and no gateway: the cable leads to one computer and nowhere else,
    // so nothing is routed through it that is not for that computer.
    const uint32_t board = myrtos_config_usb_address();
    ip4_addr_t addr, mask, gw;
    ip4_addr_set_u32(&addr, lwip_htonl(board));
    ip4_addr_set_u32(&mask, lwip_htonl(0xffffff00u));
    ip4_addr_set_zero(&gw);
    netif_add(&nif, &addr, &mask, &gw, NULL, if_init, ethernet_input);
    // The name the card gave, or "myrtos" when it gave none. The filesystem
    // server has already read it: usbdev waits for that before starting this.
    const char *host = myrtos_config_hostname();
    netif_set_hostname(&nif, host);
    netif_set_default(&nif);
    netif_set_status_callback(&nif, on_status);
    netif_set_up(&nif);
    // The LINK is left down. This interface is a cable to a host, and there is
    // not always a host: the board runs just as well on a charger, and the Mac
    // it is normally on goes to sleep. myrtos_lwip_set_link follows tud_ready
    // from the USB task, so the link says what is actually true.

    {
        extern void myrtos_dhcpd_start(struct netif *n, uint32_t board_address);
        myrtos_dhcpd_start(&nif, board);
    }

    // The responder goes up with the interface, and announces again each time
    // the link comes up -- see myrtos_lwip_set_link.
#if LWIP_MDNS_RESPONDER
    mdns_resp_init();
    if (mdns_resp_add_netif(&nif, host) == ERR_OK) {
        mdns_resp_add_service(&nif, host, "_http", DNSSD_PROTO_TCP, 80, http_txt, NULL);
        mdns_resp_announce(&nif);
        myrtos_print("net: answering to ");
        myrtos_print(host);
        myrtos_print(".local\n");
    } else {
        myrtos_print("net: the mDNS responder would not start\n");
    }
#endif

    started = true;
    const uint8_t *a = (const uint8_t *)&netif_ip4_addr(&nif)->addr;
    myrtos_print("net: lwIP up; on the cable ");
    for (int i = 0; i < 4; i++) { myrtos_print_u32(a[i]); myrtos_print(i < 3 ? "." : ""); }
    myrtos_print(", offering the computer ");
    for (int i = 0; i < 4; i++) { myrtos_print_u32(i < 3 ? a[i] : a[i] + 1u); myrtos_print(i < 3 ? "." : "\n"); }
}

// Whether there is a host on the other end of the USB cable. Called every turn
// from the USB task with tud_ready(), and cheap: it only acts on a change.
//
// This exists because lwIP used to be started only once tud_ready() was true,
// which quietly made the whole stack -- the WiFi interface included -- depend
// on a host that has nothing to do with the radio. A board on a charger came up
// with a display and a keyboard and never joined a network. The stack now
// starts as soon as the card has been read, and only THIS interface waits for
// a host, which is the only one that has any business doing so.
// Returns whether this actually changed anything, so the caller can log the
// transition and nothing else. It is called every turn; the answer is almost
// always false.
bool myrtos_lwip_set_link(bool up)
{
    if (!started) return false;
    if (up == (bool)netif_is_link_up(&nif)) return false;

    // With the link comes the name again: the computer on the other end may be
    // a different one, or the same one after a sleep, and either way it has not
    // heard the announcement made while nobody was there.
    if (up) {
        netif_set_link_up(&nif);
#if LWIP_MDNS_RESPONDER
        mdns_resp_announce(&nif);
#endif
    } else {
        netif_set_link_down(&nif);
    }
    return true;
}

// Called from the USB device task's loop, which is the one context lwIP has.
void myrtos_lwip_poll(void)
{
    if (started) sys_check_timeouts();
}

bool myrtos_lwip_started(void) { return started; }

// lwIP's own counters, which say where a packet stopped rather than that it
// did. link.recv is what reached the stack, etharp.recv what ARP saw, ip.recv
// what got past the ethernet layer, and icmp.recv what a ping reached.
#include "lwip/stats.h"
void myrtos_lwip_stats(uint32_t *out)
{
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
    out[10] = myrtos_lwip_arp_frames;
    out[11] = myrtos_lwip_ip4_frames;
    out[12] = myrtos_lwip_other_frames;
    out[13] = myrtos_lwip_for_us;
    out[14] = myrtos_lwip_icmp_frames;
    {
        extern uint32_t myrtos_lwipsock_served, myrtos_lwipsock_queued,
                        myrtos_lwipsock_taken, myrtos_lwipsock_recv,
                        myrtos_lwipsock_sent;
        out[15] = myrtos_lwipsock_served;
        out[16] = myrtos_lwipsock_queued;
        out[17] = myrtos_lwipsock_taken;
        out[18] = myrtos_lwipsock_recv;
        out[19] = myrtos_lwipsock_sent;
        extern uint32_t myrtos_lwipsock_why;
        out[20] = myrtos_lwipsock_why;
        extern uint32_t myrtos_lwipsock_lastop, myrtos_lwipsock_lastreply;
        out[21] = myrtos_lwipsock_lastop;
        out[22] = myrtos_lwipsock_lastreply;
        extern uint32_t myrtos_lwipsock_oncalls, myrtos_lwipsock_onbytes;
        out[23] = myrtos_lwipsock_oncalls;
        out[24] = myrtos_lwipsock_onbytes;
    }
    {
        // TCP's own view, which was the gap: ip and icmp were exposed and this
        // was not, so "the segment never arrived" and "TCP threw it away" have
        // been indistinguishable all along.
        out[25] = lwip_stats.tcp.recv;
        out[26] = lwip_stats.tcp.drop;
        out[27] = lwip_stats.tcp.err;
        out[28] = lwip_stats.tcp.chkerr;
        out[29] = lwip_stats.tcp.proterr;
        out[30] = lwip_stats.tcp.xmit;
    }
}

// The cable's address, host order, or 0 before lwIP has started.
uint32_t myrtos_lwip_addr(void)
{
    return started ? lwip_ntohl(netif_ip4_addr(&nif)->addr) : 0u;
}

// sys_now comes from pico_lwip_nosys, which is the reason that library is
// linked: it is the SDK's own arch glue for a build with no operating system
// under lwIP, and supplying a second one is a link error rather than a choice.
