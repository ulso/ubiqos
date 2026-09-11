// The WiFi network interface, over ESP-Hosted.
//
// The ESP32-C6 stopped being a TCP/IP stack on 10 September 2026. What crosses
// the SPI on interface number 1 is ordinary Ethernet, and this is what turns it
// into a second netif beside the one over USB.
//
// --- WHERE THIS RUNS, AND WHY IT MATTERS ------------------------------------
//
// In the USB device task, with the rest of lwIP, and nowhere else. NO_SYS is 1
// so the stack has no locking of its own, and the transport lives in a kernel
// thread of its own at priority 21 -- two contexts that must never both be
// inside lwIP. So the driver QUEUES what arrives and this drains the queue
// from the one context lwIP allows. The queue is the boundary between them and
// it is the only one.
//
// The frames do not come through read and write. Those already carry the
// control plane, and a driver module serves exactly one device, so the data
// plane goes through getstat and setstat -- see MYRTOS_SS_EH_RX and _TX.
//
// --- THE ADDRESS ------------------------------------------------------------
//
// DHCP, not AutoIP. There is a real router on this network and an address it
// hands out is one other machines can route to; the USB link keeps AutoIP,
// because the host at the other end of that one gives itself a 169.254 address
// whether or not anybody offers otherwise.
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/apps/mdns.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "io.h"
#include "config.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define EH_MTU 1500

// The MTU is what IP may put in a frame. The FRAME is that plus the ethernet
// header, and link_output sees the frame -- so a buffer sized at the MTU is
// fourteen bytes too small for every full-length segment there is.
//
// That is not an overflow, because the length was checked against the same
// wrong number: it is a silent refusal of exactly the packets that matter.
// Small ones went out and large ones did not, so httpd's headers arrived, its
// 4003-byte body never did, and the connection sat open. Ping worked
// throughout, which is what made it look like a working link.
//
// Eighteen rather than fourteen, for a VLAN tag this board will probably never
// see. Four bytes is not worth being exact about twice.
#define EH_FRAME_MAX (EH_MTU + 18u)

static struct netif wnif;
static int32_t eh_path = -1;
static bool up;

uint32_t myrtos_eh_in, myrtos_eh_out, myrtos_eh_dropped;

// The kernel owns this descriptor: it is opened once, never closed, and
// belongs to no process -- which is why it is opened with the kernel's own pid
// rather than by whoever happened to bring the radio up.
#define KERNEL_PID 0

static err_t wifi_link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len > EH_FRAME_MAX) return ERR_IF;

    // Copied out of the pbuf chain into one flat frame, because the driver
    // takes a frame and not a list of pieces, and because setstat hands the
    // bytes on immediately -- there is nothing to keep alive afterwards.
    static uint8_t flat[EH_FRAME_MAX];
    uint16_t n_copied = pbuf_copy_partial(p, flat, p->tot_len, 0);
    if (!n_copied) return ERR_IF;

    if (myrtos_io_setstat(eh_path, MYRTOS_SS_EH_TX, flat, n_copied, KERNEL_PID) < 0) {
        // One frame at a time on that wire. lwIP retries, and saying so is
        // better than dropping it quietly -- a link that loses packets without
        // counting them is the hardest kind to believe.
        myrtos_eh_dropped++;
        return ERR_IF;
    }
    myrtos_eh_out++;
    return ERR_OK;
}

static err_t wifi_if_init(struct netif *n)
{
    n->name[0] = 'w';                     // 'wl0' to anything that prints it
    n->name[1] = 'l';
    n->mtu = EH_MTU;
    n->hwaddr_len = 6;

    // The station's own address, and it has to be exactly that: the
    // co-processor turns 802.11 into 802.3 using the address the access point
    // knows, and a netif with any other would discard everything addressed to
    // the machine it is part of.
    if (myrtos_io_getstat(eh_path, MYRTOS_SS_EH_MAC, n->hwaddr, 6, KERNEL_PID) < 0)
        return ERR_IF;

    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
             | NETIF_FLAG_IGMP;
    n->output = etharp_output;
    n->linkoutput = wifi_link_output;
    return ERR_OK;
}

static void print_ip(const void *addr)
{
    const uint8_t *a = (const uint8_t *)addr;
    for (int i = 0; i < 4; i++) {
        myrtos_print_u32(a[i]);
        if (i < 3) myrtos_print(".");
    }
}

// Which interface everything that is not on a directly connected network goes
// out of.
//
// It was the USB link, because that one came up first and set itself. But that
// link has an AutoIP address and NO ROUTER: anything not on the wire itself --
// a DNS server, dn.se, the rest of the internet -- was routed into a cable
// with nowhere to go. `ping dn.se` failed as "nobody answers to that name",
// which was true and was not the reason.
//
// So the WiFi takes the default the moment it has a DHCP address, because an
// address from a server comes with a router to use it, and the USB link keeps
// it otherwise. Neither having one is the case worth complaining about, and
// the complaint belongs where somebody asked for something, not here.
static void take_the_default(struct netif *n)
{
    netif_set_default(n);
    myrtos_print("wifi: routing through the air now, not the cable\n");
}

static void on_status(struct netif *n)
{
    if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) return;

    if (netif_default != n && !ip4_addr_isany_val(*netif_ip4_gw(n))) take_the_default(n);

    // The mask and the router as well as the address, because those three
    // together are what says the address came from a DHCP server rather than
    // from somewhere else. An address on its own proves only that the stack
    // put something in the field.
    myrtos_print("wifi: address ");
    print_ip(netif_ip4_addr(n));
    myrtos_print(" mask ");
    print_ip(netif_ip4_netmask(n));
    myrtos_print(" router ");
    print_ip(netif_ip4_gw(n));
    myrtos_print("\n");
}

// Brought up when the radio is, which is when somebody has run the control
// plane far enough for the chip to know its own address. Before that there is
// no netif to make: an interface with no hardware address is one that cannot
// be talked to and cannot say why.
// Ask the driver to join the network named on the card, once.
//
// The credentials go from the file the kernel read to the driver that speaks
// to the radio, and through nothing in between. No process sees them, which is
// the whole reason the join is not in a program -- and it is also what makes
// the difference between a board that comes up on its network and one that
// waits for somebody at the keyboard, because the co-processor tears its radio
// down every time the host restarts.
static void ask_to_join(void)
{
    static bool asked;
    if (asked) return;
    if (!myrtos_config_done()) return;          // the card has not been read

    const char *creds = myrtos_config_credentials();
    asked = true;                               // once either way
    if (!creds) return;                         // no network named, or no password

    uint32_t n = 0;
    while (creds[n] || creds[n + 1]) n++;        // over the NUL between the two
    n += 2;                                     // and the pair of terminators

    if (myrtos_io_setstat(eh_path, MYRTOS_SS_EH_JOIN, creds, n, KERNEL_PID) < 0)
        myrtos_print("wifi: the driver would not take the card's network\n");
    else
        myrtos_print("wifi: joining the network named on the card\n");
}

bool myrtos_eh_netif_start(void)
{
    if (up) return true;

    if (eh_path < 0) {
        eh_path = myrtos_io_open("eh", KERNEL_PID);
        if (eh_path < 0) return false;
    }

    ask_to_join();

    uint8_t mac[6];
    if (myrtos_io_getstat(eh_path, MYRTOS_SS_EH_MAC, mac, 6, KERNEL_PID) < 0)
        return false;                     // the radio has not joined yet

    if (!netif_add(&wnif, NULL, NULL, NULL, NULL, wifi_if_init, ethernet_input))
        return false;

    // The name goes out WITH the DHCP request, in option 12, and it is the
    // only thing that tells a router what it has just given an address to.
    // Without it the board appears on the network and appears nowhere in the
    // router's list of devices -- which is exactly how it looked: pingable,
    // leased, and anonymous.
    const char *host = myrtos_config_hostname();
    netif_set_hostname(&wnif, host);

    netif_set_status_callback(&wnif, on_status);
    netif_set_up(&wnif);
    netif_set_link_up(&wnif);
    dhcp_start(&wnif);

    // And the responder on this interface too. mDNS was registered on the USB
    // netif alone, so the board answered to its name over the cable and only
    // by address over the air -- the same name, and half a machine could find
    // it.
#if LWIP_MDNS_RESPONDER
    if (mdns_resp_add_netif(&wnif, host) == ERR_OK) {
        mdns_resp_add_service(&wnif, host, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
        mdns_resp_announce(&wnif);
    } else {
        myrtos_print("wifi: the mDNS responder would not take this interface\n");
    }
#endif

    up = true;
    myrtos_print("wifi: interface up as ");
    myrtos_print(host);
    myrtos_print(", asking DHCP for an address\n");
    return true;
}

bool myrtos_eh_netif_up(void) { return up; }

// Everything the driver has queued, handed to lwIP. Called from the USB task's
// loop, which is the only place either of these may be touched.
//
// Bounded, and deliberately: a burst of broadcast traffic is not a reason to
// stop answering USB for as long as it lasts. What is left waits for the next
// turn, which is a millisecond away.
void myrtos_eh_netif_poll(void)
{
    if (!up) return;

    for (int budget = 0; budget < 8; budget++) {
        static uint8_t frame[EH_FRAME_MAX];
        int32_t n = myrtos_io_getstat(eh_path, MYRTOS_SS_EH_RX,
                                      frame, sizeof(frame), KERNEL_PID);
        if (n <= 0) return;

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (uint16_t)n, PBUF_POOL);
        if (!p) { myrtos_eh_dropped++; continue; }   // the pool is empty; drop it
        pbuf_take(p, frame, (uint16_t)n);

        myrtos_eh_in++;
        if (wnif.input(p, &wnif) != ERR_OK) {
            pbuf_free(p);
            myrtos_eh_dropped++;
        }
    }
}
