// A DHCP server for the USB cable, and for nothing else.
//
// The cable used to be link-local: the board took a 169.254 address with
// AutoIP and so did the computer. That works on a computer with one network,
// and on one with more it is a coin toss. 169.254.0.0/16 is the same subnet on
// every interface, and macOS routes it through ONE of them -- the primary, not
// the cable. Measured on 18 Sep 2026 with the Mac on wired ethernet and WiFi:
//
//     169.254            link#14   en15       (the wired network)
//     169.254            link#16   en0        (the WiFi)
//     169.254.91.150     link#14   en15       the board, on the wrong wire
//
// en27, the board's own interface, had its 169.254 address and no route at
// all. So a connection to the board went out on the ethernet, ARP there was
// never answered, and it timed out -- except when something had happened to
// clone a route through en27 first, which is why it looked like "the first
// request after httpd starts". It was the first after every reboot of the
// board, which is when the cable's routes are thrown away.
//
// A subnet of its own for the cable makes the route unambiguous on any
// computer: the board is 192.168.7.1 -- or what /sd/config.txt says as
// usb_address -- and it offers the computer the next address up, on a /24.
// No router and no DNS server are offered. The board is not a way to the
// internet, and a computer told otherwise would try to send everything through
// it; the name is still found by mDNS.
//
// One client, one address, and no lease table: there is only ever one computer
// at the other end of a USB cable. A computer that asks for some other address
// -- the one another board gave it yesterday -- is told no, and asks again.
#include <stdint.h>
#include <string.h>

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

void ubiqos_print(const char *s);

#define SERVER_PORT 67
#define CLIENT_PORT 68
#define LEASE_S     86400u            // a day; the cable decides, not the lease

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE, INFORM };

// Where the fields are in a BOOTP message, and where the options start.
#define OP        0
#define XID       4
#define FLAGS     10
#define CIADDR    12
#define YIADDR    16
#define SIADDR    20
#define GIADDR    24
#define CHADDR    28
#define COOKIE    236
#define OPTIONS   240
#define REPLY_LEN 300                 // BOOTP's minimum; some clients insist

static struct udp_pcb *pcb;
static struct netif *cable;
static uint32_t board, client;        // host byte order

uint32_t ubiqos_dhcpd_offers, ubiqos_dhcpd_acks, ubiqos_dhcpd_naks;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// One option's value, or null. A pad is one byte, the end ends it, and a
// length that runs past the message ends it too.
static const uint8_t *option(const uint8_t *o, uint32_t n, uint8_t code, uint8_t *len)
{
    for (uint32_t i = 0; i < n; ) {
        if (o[i] == 0) { i++; continue; }
        if (o[i] == 255 || i + 1 >= n) break;
        const uint8_t l = o[i + 1];
        if (i + 2u + l > n) break;
        if (o[i] == code) { *len = l; return o + i + 2; }
        i += 2u + l;
    }
    return 0;
}

// The answer, broadcast: until it has one, the computer has no address to
// send it to.
static void reply(const uint8_t *req, uint8_t type, bool give_address)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, REPLY_LEN, PBUF_RAM);
    if (!p) return;
    uint8_t *m = (uint8_t *)p->payload;
    memset(m, 0, REPLY_LEN);

    m[OP] = 2;                        // BOOTREPLY
    m[1] = 1;                         // ethernet
    m[2] = 6;                         // its address length
    memcpy(m + XID, req + XID, 4);
    memcpy(m + FLAGS, req + FLAGS, 2);
    memcpy(m + CIADDR, req + CIADDR, 4);
    if (give_address) put32(m + YIADDR, client);
    put32(m + SIADDR, board);
    memcpy(m + GIADDR, req + GIADDR, 4);
    memcpy(m + CHADDR, req + CHADDR, 16);
    m[COOKIE] = 99; m[COOKIE + 1] = 130; m[COOKIE + 2] = 83; m[COOKIE + 3] = 99;

    uint8_t *o = m + OPTIONS;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 54; *o++ = 4; put32(o, board); o += 4;          // who is answering
    if (type != NAK) {
        *o++ = 1;  *o++ = 4; put32(o, 0xffffff00u); o += 4;  // a /24
        if (give_address) { *o++ = 51; *o++ = 4; put32(o, LEASE_S); o += 4; }
    }
    *o = 255;

    udp_sendto_if(pcb, p, IP4_ADDR_BROADCAST, CLIENT_PORT, cable);
    pbuf_free(p);
}

static void on_request(void *arg, struct udp_pcb *u, struct pbuf *p,
                       const ip_addr_t *from, u16_t port)
{
    (void)arg; (void)u; (void)from; (void)port;
    uint8_t m[576];
    const uint32_t n = pbuf_copy_partial(p, m, sizeof m, 0);
    pbuf_free(p);

    if (n < OPTIONS + 3 || m[OP] != 1 || m[1] != 1 || m[2] != 6) return;
    if (m[COOKIE] != 99 || m[COOKIE + 1] != 130 || m[COOKIE + 2] != 83 || m[COOKIE + 3] != 99)
        return;

    uint8_t len = 0;
    const uint8_t *t = option(m + OPTIONS, n - OPTIONS, 53, &len);
    if (!t || len != 1) return;

    switch (t[0]) {
    case DISCOVER:
        reply(m, OFFER, true);
        ubiqos_dhcpd_offers++;
        break;
    case REQUEST: {
        // Answering somebody else's offer: not ours to say anything about.
        const uint8_t *sid = option(m + OPTIONS, n - OPTIONS, 54, &len);
        if (sid && len == 4 && get32(sid) != board) return;
        const uint8_t *want = option(m + OPTIONS, n - OPTIONS, 50, &len);
        const uint32_t asked = (want && len == 4) ? get32(want) : get32(m + CIADDR);
        if (asked && asked != client) { reply(m, NAK, false); ubiqos_dhcpd_naks++; break; }
        reply(m, ACK, true);
        ubiqos_dhcpd_acks++;
        break;
    }
    case INFORM:
        reply(m, ACK, false);         // the settings, for an address it already has
        break;
    default:
        break;                        // RELEASE and DECLINE change nothing here
    }
}

// Started once, with the cable's interface. Its address is already set.
void ubiqos_dhcpd_start(struct netif *n, uint32_t board_address)
{
    if (pcb) return;
    cable = n;
    board = board_address;
    client = board_address + 1u;
    pcb = udp_new();
    if (!pcb) { ubiqos_print("net: no room for the DHCP server\n"); return; }
    udp_bind_netif(pcb, n);
    if (udp_bind(pcb, IP4_ADDR_ANY, SERVER_PORT) != ERR_OK) {
        udp_remove(pcb);
        pcb = 0;
        ubiqos_print("net: the DHCP server could not have port 67\n");
        return;
    }
    udp_recv(pcb, on_request, 0);
}
