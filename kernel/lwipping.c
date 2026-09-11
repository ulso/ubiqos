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
#include "lwip/timeouts.h"
#include "../common/myrtos_abi.h"

void myrtos_print(const char *s);

#define PING_DATA 32u                   // payload, so a reply is worth timing
#define PING_ID   0xbeefu

static struct raw_pcb *pcb;
static volatile uint32_t seq;

// What the caller asks about. One ping at a time: this is a command somebody
// typed, not a service, and two at once would be two people.
static struct {
    uint32_t state;                     // MYRTOS_PING_*
    ip_addr_t addr;
    uint32_t sent_us;
    uint32_t took_us;
    uint32_t want_seq;
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

    if (cur.state != MYRTOS_PING_WAITING) return 0;    // not ours to take

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
    cur.state = MYRTOS_PING_REPLIED;

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
    cur.state    = MYRTOS_PING_WAITING;

    err_t rc = raw_sendto(pcb, p, &cur.addr);
    pbuf_free(p);
    if (rc != ERR_OK) { cur.state = MYRTOS_PING_UNREACHABLE; return false; }
    return true;
}

// The name came back. Called by lwIP from its own context, which is this task.
static void on_resolved(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (cur.state != MYRTOS_PING_RESOLVING) return;
    if (!addr) { cur.state = MYRTOS_PING_NONAME; return; }
    cur.addr = *addr;
    send_echo();
}

// --- WHAT THE SERVER CALLS --------------------------------------------------

int32_t myrtos_ping_start(const char *host)
{
    if (!pcb) {
        pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb) return -1;
        raw_recv(pcb, on_icmp, NULL);
        if (raw_bind(pcb, IP_ADDR_ANY) != ERR_OK) { raw_remove(pcb); pcb = NULL; return -1; }
    }

    cur.state   = MYRTOS_PING_RESOLVING;
    cur.took_us = 0;
    ip_addr_set_zero(&cur.addr);

    // A literal is not a question for anybody. dns_gethostbyname answers one
    // straight away too, but going through it for `ping 192.168.68.1` would
    // put a name lookup in the path of an address that is already an address.
    if (ipaddr_aton(host, &cur.addr)) return send_echo() ? 0 : -1;

    err_t rc = dns_gethostbyname(host, &cur.addr, on_resolved, NULL);
    if (rc == ERR_OK) return send_echo() ? 0 : -1;      // already known
    if (rc == ERR_INPROGRESS) return 0;                 // asked; on_resolved follows
    cur.state = MYRTOS_PING_NONAME;
    return -1;
}

// Three words: where it is now, the address once known, and the microseconds
// when there is an answer to time.
void myrtos_ping_poll(uint32_t out[3])
{
    // The deadline is checked HERE rather than on a timer, because this is
    // asked once a turn anyway and a timeout that needs its own callback is a
    // callback that can outlive the thing it was timing.
    if (cur.state == MYRTOS_PING_WAITING) {
        uint32_t waited = (uint32_t)time_us_64() - cur.sent_us;
        if (waited > 2000000u) cur.state = MYRTOS_PING_TIMEDOUT;
    }
    out[0] = cur.state;
    out[1] = ip_addr_get_ip4_u32(&cur.addr);
    out[2] = cur.took_us;
}
