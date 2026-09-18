// ping, from the board.
//
// ICMP echo over lwIP's raw API, with names ending in .local asked for by
// multicast so that `ping fruit-jam.local` works here the same way it works
// from the Mac. The responder that answers such questions has been running
// since the 9th; this is the half that asks them.
//
// --- WHY IT IS IN THREE PIECES ----------------------------------------------
//
// lwIP may be touched from the USB device task and nowhere else, and that task
// must not sit still: it drives the console, the network and the socket server
// on the same turn. So nothing here waits for anything.
//
// A ping is started, and then polled. The starting resolves the name if it
// needs to and sends one echo; the polling says what has happened since. The
// process that typed `ping` does the waiting, which is the one place in the
// system where waiting is free.
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/dns.h"

int32_t ubiqos_mdns_resolve(const char *name);
int32_t ubiqos_mdns_state(uint32_t *addr_out);
#include "lwip/timeouts.h"
#include "../common/ubiqos_abi.h"

void ubiqos_print(const char *s);

#define PING_DATA 32u                   // payload, so a reply is worth timing
#define PING_ID   0xbeefu

static struct raw_pcb *pcb;
static volatile uint32_t seq;

// What the caller asks about. One ping at a time: this is a command somebody
// typed, not a service, and two at once would be two people.
static struct {
    uint32_t state;                     // UBIQOS_PING_*
    ip_addr_t addr;
    uint32_t sent_us;
    uint32_t took_us;
    uint32_t want_seq;
    bool     by_dns;                    // a DNS server is answering, not mDNS
} cur;

uint64_t time_us_64(void);

// --- THE REPLY --------------------------------------------------------------
//
// A raw PCB is handed the whole datagram, IP header included, so the ICMP
// starts one header in -- and the header's length is in its own first byte,
// which is why it cannot simply be assumed to be twenty.
static u8_t on_icmp(void *arg, struct raw_pcb *p, struct pbuf *buf, const ip_addr_t *from)
{
    (void)arg; (void)p;

    if (cur.state != UBIQOS_PING_WAITING) return 0;    // not ours to take

    u8_t hlen = 0;
    if (buf->len >= 1) hlen = (u8_t)((*(u8_t *)buf->payload & 0x0f) * 4);
    if (hlen < 20 || buf->tot_len < hlen + (u16_t)sizeof(struct icmp_echo_hdr)) return 0;

    struct icmp_echo_hdr hdr;
    if (pbuf_copy_partial(buf, &hdr, sizeof(hdr), hlen) != sizeof(hdr)) return 0;

    if (hdr.type != ICMP_ER) return 0;                 // not an echo reply
    if (lwip_ntohs(hdr.id) != PING_ID) return 0;       // somebody else's ping
    if (lwip_ntohs(hdr.seqno) != cur.want_seq) return 0;
    if (!ip_addr_cmp(from, &cur.addr)) return 0;

    cur.took_us = (uint32_t)time_us_64() - cur.sent_us;
    cur.state = UBIQOS_PING_REPLIED;

    pbuf_free(buf);
    return 1;                                          // eaten
}

static bool send_echo(void)
{
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + PING_DATA, PBUF_RAM);
    if (!p) return false;

    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->chksum = 0;
    e->id     = lwip_htons(PING_ID);
    e->seqno  = lwip_htons((u16_t)++seq);

    // Something recognisable rather than whatever the pool held, so a capture
    // shows at a glance which packets are ours.
    char *data = (char *)e + sizeof(struct icmp_echo_hdr);
    for (uint32_t i = 0; i < PING_DATA; i++) data[i] = (char)('a' + (i % 26));

    e->chksum = inet_chksum(e, (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA));

    cur.want_seq = seq;
    cur.sent_us  = (uint32_t)time_us_64();
    cur.state    = UBIQOS_PING_WAITING;

    err_t rc = raw_sendto(pcb, p, &cur.addr);
    pbuf_free(p);
    if (rc != ERR_OK) { cur.state = UBIQOS_PING_UNREACHABLE; return false; }
    return true;
}

// A DNS server answered. Called by lwIP from its own context, which is this
// task, so there is nothing to hand over.
static void on_resolved(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (cur.state != UBIQOS_PING_RESOLVING) return;
    if (!addr) { cur.state = UBIQOS_PING_NONAME; return; }
    cur.addr = *addr;
    send_echo();
}

// --- WHAT THE SERVER CALLS --------------------------------------------------

int32_t ubiqos_ping_start(const char *host)
{
    if (!pcb) {
        pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb) return -1;
        raw_recv(pcb, on_icmp, NULL);
        if (raw_bind(pcb, IP_ADDR_ANY) != ERR_OK) { raw_remove(pcb); pcb = NULL; return -1; }
    }

    cur.state   = UBIQOS_PING_RESOLVING;
    cur.took_us = 0;
    ip_addr_set_zero(&cur.addr);

    // A literal is not a question for anybody.
    if (ipaddr_aton(host, &cur.addr)) return send_echo() ? 0 : -1;

    // Which kind of name it is decides who is asked, and the two are different
    // services entirely: .local is answered by whoever holds the name, on the
    // spot; everything else is answered by a DNS server the router told us
    // about. Asking the wrong one gets a perfectly confident "nobody answers
    // to that name", which is what `ping dn.se` used to say.
    bool dotted = false, dot_local = false;
    {
        uint32_t n = 0;
        while (host[n]) n++;
        for (uint32_t i = 0; i < n; i++) if (host[i] == '.') dotted = true;
        if (n > 6) {
            const char *t = host + n - 6;
            dot_local = t[0] == '.' && (t[1] == 'l' || t[1] == 'L')
                                    && (t[2] == 'o' || t[2] == 'O')
                                    && (t[3] == 'c' || t[3] == 'C')
                                    && (t[4] == 'a' || t[4] == 'A')
                                    && (t[5] == 'l' || t[5] == 'L');
        }
    }

    if (dotted && !dot_local) {
        // The ordinary internet. lwIP's DNS client, with the servers DHCP
        // handed over -- which only works at all now that the interface with a
        // router on it is the default one.
        err_t rc = dns_gethostbyname(host, &cur.addr, on_resolved, NULL);
        if (rc == ERR_OK) return send_echo() ? 0 : -1;      // already known
        if (rc == ERR_INPROGRESS) { cur.by_dns = true; return 0; }
        cur.state = UBIQOS_PING_NONAME;
        return -1;
    }

    // And a name goes to our own querier rather than to lwIP's DNS client.
    //
    // That client does resolve .local names -- it is one #define -- but it
    // sends the question on the DEFAULT netif only. The default here is the
    // USB link, so `ping rpi50.local` asked the Mac about a Raspberry Pi that
    // is on the air, and was told nothing by everybody. Ours asks on every
    // interface that is up.
    // A name with no dot in it at all is the one people type for the machine in
    // the next room, and .local is what they mean.
    static char with_local[80];
    const char *ask = host;
    if (!dotted) {
        uint32_t n = 0;
        while (host[n] && n < sizeof(with_local) - 7) { with_local[n] = host[n]; n++; }
        const char *t = ".local";
        for (int i = 0; i < 6; i++) with_local[n++] = t[i];
        with_local[n] = 0;
        ask = with_local;
    }

    cur.by_dns = false;
    if (ubiqos_mdns_resolve(ask) < 0) { cur.state = UBIQOS_PING_NONAME; return -1; }
    return 0;                                           // the poll picks it up
}

// Three words: where it is now, the address once known, and the microseconds
// when there is an answer to time.
void ubiqos_ping_poll(uint32_t out[3])
{
    // The deadline is checked HERE rather than on a timer, because this is
    // asked once a turn anyway and a timeout that needs its own callback is a
    // callback that can outlive the thing it was timing.
    // Still waiting on a name. The querier answers or gives up on its own, and
    // the echo goes out the moment it answers.
    if (cur.state == UBIQOS_PING_RESOLVING && !cur.by_dns) {
        uint32_t a = 0;
        int32_t r = ubiqos_mdns_state(&a);
        if (r < 0) cur.state = UBIQOS_PING_NONAME;
        else if (r > 0) {
            ip_addr_set_ip4_u32(&cur.addr, a);
            send_echo();
        }
    }

    if (cur.state == UBIQOS_PING_WAITING) {
        uint32_t waited = (uint32_t)time_us_64() - cur.sent_us;
        if (waited > 2000000u) cur.state = UBIQOS_PING_TIMEDOUT;
    }
    out[0] = cur.state;
    out[1] = ip_addr_get_ip4_u32(&cur.addr);
    out[2] = cur.took_us;
}
