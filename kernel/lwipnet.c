// lwIP on the USB network device.
//
// One context and only one: NO_SYS = 1 means lwIP has no locking of its own,
// so everything here is called from the USB device task and from nowhere else.
// A frame arrives in tud_network_recv_cb, which runs there, and goes straight
// into the netif -- no queue, because TinyUSB already has one: returning false
// from the callback means "ask me again", which is the flow control.
//
// The address comes from AutoIP. There is one host on the other end of this
// link and it gives itself a 169.254 address whether or not anybody offers
// DHCP, so a server would be answering a question nobody asked.
#include <stdint.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/autoip.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
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
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = link_output;
    return ERR_OK;
}

static void on_status(struct netif *n)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(n))) {
        const uint8_t *a = (const uint8_t *)&netif_ip4_addr(n)->addr;
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
    netif_add(&nif, NULL, NULL, NULL, NULL, if_init, ethernet_input);
    netif_set_hostname(&nif, "myrtos");
    netif_set_default(&nif);
    netif_set_status_callback(&nif, on_status);
    netif_set_up(&nif);
    netif_set_link_up(&nif);
    autoip_start(&nif);

    started = true;
    myrtos_print("net: lwIP up, asking AutoIP for an address\n");
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
}

// The address AutoIP settled on, host order, or 0 while it is still deciding.
uint32_t myrtos_lwip_addr(void)
{
    return started ? lwip_ntohl(netif_ip4_addr(&nif)->addr) : 0u;
}

// sys_now comes from pico_lwip_nosys, which is the reason that library is
// linked: it is the SDK's own arch glue for a build with no operating system
// under lwIP, and supplying a second one is a link error rather than a choice.
