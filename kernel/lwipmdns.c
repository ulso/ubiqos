// Asking the network questions: names, and what is on it.
//
// lwIP's mdns.c is a RESPONDER. It answers "who is fruit-jam" and cannot ask.
// LWIP_DNS_SUPPORT_MDNS_QUERIES makes the DNS client ask for .local names, and
// that is what `ping usmbp6.local` used -- but it goes out on the DEFAULT netif
// and nowhere else, so `ping rpi50.local` failed on a board whose default is
// the USB link while the Pi is on the air. The name was fine; the question
// never reached the network it was about.
//
// So the asking is done here, on a netif named by the caller, and both netifs
// are tried. The same question with a different type is also how a service is
// browsed, so one small querier answers both:
//
//     A   (1)   what address does this name have
//     PTR (12)  what answers to this service
//
// --- WHY IT DOES NOT BIND PORT 5353 -----------------------------------------
//
// The responder has it, and two pcbs on one port is a fight. RFC 6762 has the
// way round: a one-shot query may set the UNICAST-RESPONSE bit in the question
// and be answered directly, at whatever port it came from. So this sends from
// an ephemeral port and is replied to there, and the responder never notices.
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip.h"          // ip_current_netif, inside a recv callback
#include "../common/myrtos_abi.h"

void myrtos_print(const char *s);
uint64_t time_us_64(void);

#define MDNS_PORT 5353u
#define QTYPE_A   1u
#define QTYPE_PTR 12u
#define QCLASS_IN_UNICAST 0x8001u       // IN, and answer me directly

static struct udp_pcb *pcb;

// What the current question is and what has come back. One at a time, because
// these are commands somebody typed rather than a service.
static struct {
    bool     open;                      // a question is outstanding
    uint16_t qtype;
    char     want[64];                  // the name asked about, lower case
    uint32_t started_us;

    uint32_t addr;                      // A: the best answer so far
    uint32_t rank;                      // and how good it is -- see address_rank
    bool     got_addr;

    char     found[MYRTOS_MDNS_MAX][64];   // PTR: what answered
    uint32_t nfound;
} q;

// --- READING A DNS NAME -----------------------------------------------------
//
// Labels, each a length byte and that many characters, ending at a zero -- and
// any of them may instead be a POINTER back into the message, which is what
// makes this worth a function. A pointer is two bytes with the top two bits
// set, and the rest an offset from the start of the message.
//
// Following one moves the reading but not the CURSOR: after a pointer the
// caller carries on two bytes further, not wherever the name ended. Getting
// that wrong reads the same answer for ever, which is a hang rather than a
// wrong answer.
static uint32_t read_name(const uint8_t *msg, uint32_t len, uint32_t at,
                          char *out, uint32_t cap)
{
    uint32_t n = 0, hops = 0;
    uint32_t cursor = 0;                // where the caller carries on, 0 = here

    while (at < len) {
        uint8_t l = msg[at];

        if ((l & 0xc0u) == 0xc0u) {     // a pointer
            if (at + 1 >= len) break;
            if (!cursor) cursor = at + 2;
            at = (uint32_t)((l & 0x3fu) << 8 | msg[at + 1]);
            if (++hops > 8) break;      // a loop, and eight is already generous
            continue;
        }

        at++;
        if (!l) break;                  // the root: the name is done
        if (at + l > len) break;

        if (n && n + 1 < cap) out[n++] = '.';
        for (uint8_t i = 0; i < l && n + 1 < cap; i++) {
            char c = (char)msg[at + i];
            out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        at += l;
    }
    out[n < cap ? n : cap - 1] = 0;
    return cursor ? cursor : at;
}

static bool same_name(const char *a, const char *b)
{
    for (;;) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        if (!x) return true;
        a++; b++;
    }
}

// Which of several addresses to believe.
//
// A machine on two networks answers with an address for each, and rpi50
// answered with both 192.168.68.59 and 169.254.175.99 in one frame. Taking the
// first took the link-local one, which nothing here can reach: the echo went
// out and was never coming back, and the failure read as an unreachable host
// rather than a badly chosen address.
//
// The first attempt at choosing asked whether the address was on the same
// subnet as ANY of our interfaces, and that was worse than useless: the USB
// link is 169.254.x.x with a /16, so every link-local address on earth looks
// local to it. It ranked the Pi's unreachable address highest, for a reason
// that was perfectly correct arithmetic.
//
// The question that works is not what the address looks like. It is which
// interface the answer ARRIVED ON, and whether the address is reachable there.
// A reply that came over the air carrying a link-local address is telling us
// about a link we are not on.
static uint32_t address_rank(uint32_t a)
{
    const struct netif *n = ip_current_netif();
    if (!n) return 1;

    uint32_t mine = ip4_addr_get_u32(netif_ip4_addr(n));
    uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(n));
    if (!mine || !mask) return 1;

    return ((a ^ mine) & mask) == 0 ? 3u : 1u;
}

static void on_reply(void *arg, struct udp_pcb *p, struct pbuf *buf,
                     const ip_addr_t *from, u16_t port)
{
    (void)arg; (void)p; (void)from; (void)port;

    if (!q.open || buf->tot_len < 12) { pbuf_free(buf); return; }

    static uint8_t msg[512];
    uint16_t len = buf->tot_len > sizeof(msg) ? (uint16_t)sizeof(msg) : buf->tot_len;
    pbuf_copy_partial(buf, msg, len, 0);
    pbuf_free(buf);

    uint32_t qd = (uint32_t)(msg[4] << 8 | msg[5]);
    uint32_t an = (uint32_t)(msg[6] << 8 | msg[7]);
    uint32_t at = 12;

    char name[64];
    for (uint32_t i = 0; i < qd && at < len; i++) {
        at = read_name(msg, len, at, name, sizeof(name));
        at += 4;                        // qtype and qclass
    }

    for (uint32_t i = 0; i < an && at + 10 <= len; i++) {
        at = read_name(msg, len, at, name, sizeof(name));
        if (at + 10 > len) break;
        uint16_t type = (uint16_t)(msg[at] << 8 | msg[at + 1]);
        uint16_t rdlen = (uint16_t)(msg[at + 8] << 8 | msg[at + 9]);
        uint32_t rd = at + 10;
        at = rd + rdlen;
        if (at > len) break;

        if (q.qtype == QTYPE_A && type == QTYPE_A && rdlen == 4) {
            // The answer has to be about what was asked. A responder may put
            // several records in one frame, and taking the first A record in
            // the packet is how you get somebody else's address.
            if (!same_name(name, q.want)) continue;
            uint32_t a = (uint32_t)msg[rd] | ((uint32_t)msg[rd + 1] << 8)
                       | ((uint32_t)msg[rd + 2] << 16) | ((uint32_t)msg[rd + 3] << 24);
            uint32_t rank = address_rank(a);
            if (!q.got_addr || rank > q.rank) { q.addr = a; q.rank = rank; q.got_addr = true; }
            // Not done yet unless this one is on our own network: a better
            // answer may be later in the same frame, and usually is.
            if (rank >= 3) { q.open = false; return; }
            continue;
        }

        if (q.qtype == QTYPE_PTR && type == QTYPE_PTR) {
            if (!same_name(name, q.want)) continue;
            if (q.nfound >= MYRTOS_MDNS_MAX) continue;
            char inst[64];
            read_name(msg, len, rd, inst, sizeof(inst));
            for (uint32_t k = 0; k < q.nfound; k++)
                if (same_name(q.found[k], inst)) goto next;   // a repeat
            for (uint32_t c = 0; c < sizeof(inst); c++) q.found[q.nfound][c] = inst[c];
            q.nfound++;
        }
next:   ;
    }
}

// The question, as bytes: a header of zeroes with one question in it, then the
// name in labels, then the type and the class with the unicast bit set.
static uint32_t build(uint8_t *out, const char *name, uint16_t qtype)
{
    uint32_t n = 0;
    for (int i = 0; i < 12; i++) out[n++] = 0;
    out[5] = 1;                         // one question

    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        uint32_t l = (uint32_t)(dot - p);
        if (!l || l > 63) return 0;
        out[n++] = (uint8_t)l;
        for (uint32_t i = 0; i < l; i++) out[n++] = (uint8_t)p[i];
        p = *dot ? dot + 1 : dot;
    }
    out[n++] = 0;

    out[n++] = (uint8_t)(qtype >> 8);       out[n++] = (uint8_t)qtype;
    out[n++] = (uint8_t)(QCLASS_IN_UNICAST >> 8);
    out[n++] = (uint8_t)QCLASS_IN_UNICAST;
    return n;
}

// On EVERY interface, which is the whole point: a question about a name only
// reaches the network that name is on.
static void ask_everywhere(const uint8_t *msg, uint32_t len)
{
    ip_addr_t group;
    IP4_ADDR(ip_2_ip4(&group), 224, 0, 0, 251);

    struct netif *n;
    NETIF_FOREACH(n) {
        if (!netif_is_up(n) || !netif_is_link_up(n)) continue;

        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) continue;
        pbuf_take(p, msg, (u16_t)len);
        udp_sendto_if(pcb, p, &group, MDNS_PORT, n);
        pbuf_free(p);
    }
}

static bool ensure_pcb(void)
{
    if (pcb) return true;
    pcb = udp_new();
    if (!pcb) return false;
    // Any port. Binding 5353 would fight the responder for it, and a one-shot
    // question does not need to be there -- it asks to be answered directly.
    if (udp_bind(pcb, IP_ADDR_ANY, 0) != ERR_OK) { udp_remove(pcb); pcb = NULL; return false; }
    udp_recv(pcb, on_reply, NULL);
    return true;
}

static int32_t start(const char *name, uint16_t qtype)
{
    if (!ensure_pcb()) return -1;

    q.open = true;
    q.qtype = qtype;
    q.got_addr = false;
    q.addr = 0;
    q.rank = 0;
    q.nfound = 0;
    q.started_us = (uint32_t)time_us_64();

    uint32_t k = 0;
    while (name[k] && k < sizeof(q.want) - 1) {
        char c = name[k];
        q.want[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        k++;
    }
    q.want[k] = 0;

    uint8_t msg[128];
    uint32_t len = build(msg, q.want, qtype);
    if (!len) { q.open = false; return -1; }

    ask_everywhere(msg, len);
    return 0;
}

int32_t myrtos_mdns_resolve(const char *name) { return start(name, QTYPE_A); }
int32_t myrtos_mdns_browse(const char *service) { return start(service, QTYPE_PTR); }

// 0 still asking, 1 an answer, -1 nobody said anything in time.
int32_t myrtos_mdns_state(uint32_t *addr_out)
{
    // A first-rate answer ends the question at once; a second-rate one waits a
    // moment in case something better is on its way, and is then taken.
    if (q.got_addr && (q.rank >= 3 || !q.open ||
                       (uint32_t)time_us_64() - q.started_us > 400000u)) {
        q.open = false;
        if (addr_out) *addr_out = q.addr;
        return 1;
    }
    if (q.qtype == QTYPE_PTR && q.nfound) {
        // A browse has no single answer, so it runs to the deadline and takes
        // whatever arrived: one responder answering quickly is not a reason to
        // stop listening to the rest.
        if ((uint32_t)time_us_64() - q.started_us < 1500000u) return 0;
        q.open = false;
        return 1;
    }
    if ((uint32_t)time_us_64() - q.started_us > 1500000u) { q.open = false; return -1; }
    return 0;
}

uint32_t myrtos_mdns_found(uint32_t i, char *out, uint32_t cap)
{
    if (i >= q.nfound) return 0;
    uint32_t n = 0;
    while (q.found[i][n] && n + 1 < cap) { out[n] = q.found[i][n]; n++; }
    out[n] = 0;
    return n;
}
