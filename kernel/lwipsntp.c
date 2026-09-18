// Asking the network what time it is.
//
// SNTP, written here rather than taken from lwIP's apps. Theirs is a full
// client -- several servers, kiss-o'-death handling, its own retry policy,
// options for all of it -- and it cost 4 kB of SRAM, which took the RISC-V
// chargen build below the heap the SDK needs to boot. What is actually needed
// is one question and one answer, and that is what this is. The same trade as
// kernel/lwipmdns.c, for the same reason.
//
// The query goes out once there is a route out. That is later than "an
// interface exists": the CDC-NCM side is a link-local cable to one host with no
// gateway, so a packet sent that way goes nowhere. Waiting for a default route
// means waiting for WiFi and its lease, which is the only way off this board.
//
// Like everything else that touches lwIP, this runs in the USB device task and
// nowhere else.

#include <stdint.h>
#include <stdbool.h>
#include "clock.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "pico/time.h"

void ubiqos_print(const char *s);

#define NTP_PORT        123u
#define NTP_PACKET      48u
#define NTP_STAMP_AT    40u       // the transmit timestamp, seconds first

// NTP counts from 1900, Unix from 1970. Seventy years, seventeen of them leap.
#define NTP_TO_UNIX     2208988800u

// How often to ask. Once an hour once answered, every half minute until then:
// the first answer is what a log wants and the rest is drift.
#define ASK_AGAIN_US    (3600ull * 1000000ull)
#define RETRY_US        (30ull * 1000000ull)
#define RESOLVE_AGAIN_US (2ull * 1000000ull)

static struct udp_pcb *pcb;
static ip_addr_t server;
static bool have_server;
static uint64_t asked_at;

static void took_it(uint32_t ntp_seconds)
{
    if (ntp_seconds < NTP_TO_UNIX) return;         // before 1970: not an answer

    const bool first = !ubiqos_clock_is_set();
    ubiqos_clock_set(ntp_seconds - NTP_TO_UNIX);

    if (first) {
        char stamp[24];
        ubiqos_clock_stamp(stamp, sizeof stamp);
        ubiqos_print("ntp: the time is ");
        ubiqos_print(stamp);
        ubiqos_print(ubiqos_clock_offset() ? "\n" : " UTC\n");
    }
}

static void on_reply(void *arg, struct udp_pcb *p, struct pbuf *pb,
                     const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)p; (void)addr; (void)port;
    if (!pb) return;

    uint8_t b[4];
    if (pb->tot_len >= NTP_PACKET &&
        pbuf_copy_partial(pb, b, 4, NTP_STAMP_AT) == 4) {
        took_it(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                ((uint32_t)b[2] << 8)  |  (uint32_t)b[3]);
    }
    pbuf_free(pb);
}

static void on_resolved(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (!addr) return;                             // ask again on the next turn
    server = *addr;
    have_server = true;
}

static void ask(void)
{
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, NTP_PACKET, PBUF_RAM);
    if (!pb) return;

    uint8_t *q = (uint8_t *)pb->payload;
    for (uint32_t i = 0; i < NTP_PACKET; i++) q[i] = 0;
    q[0] = 0x23;                                   // no leap warning, v4, client

    udp_sendto(pcb, pb, &server, NTP_PORT);
    pbuf_free(pb);
    have_server = false;                           // resolved again next round
}

// Whether there is a way off this board: a default interface, up, with a
// gateway. Link-local on its own is not one.
static bool have_a_route(void)
{
    struct netif *n = netif_default;
    if (!n || !netif_is_up(n) || !netif_is_link_up(n)) return false;
    return !ip4_addr_isany_val(*netif_ip4_gw(n));
}

void ubiqos_sntp_poll(void)
{
    if (!have_a_route()) return;

    if (!pcb) {
        pcb = udp_new();
        if (!pcb) return;
        if (udp_bind(pcb, IP_ANY_TYPE, 0) != ERR_OK) { udp_remove(pcb); pcb = 0; return; }
        udp_recv(pcb, on_reply, 0);
        ubiqos_print("ntp: asking pool.ntp.org what time it is\n");
    }

    const uint64_t now = time_us_64();
    const uint64_t due = ubiqos_clock_is_set() ? ASK_AGAIN_US : RETRY_US;
    if (asked_at && now - asked_at < due) return;

    if (!have_server) {
        // Resolved fresh each round: the pool answers with a different server
        // every time it is asked, and holding on to one would quietly turn a
        // pool into a single host somebody else is paying for.
        //
        // A lookup that has only been STARTED is not an attempt, and must not
        // book one: doing that put the first answer 30 seconds after boot,
        // because the round that kicked off the lookup consumed the slot and
        // the reply arrived with nobody left to use it. Its own short throttle
        // instead, so the resolver is not asked once a millisecond either.
        static uint64_t resolved_at;
        if (resolved_at && now - resolved_at < RESOLVE_AGAIN_US) return;
        resolved_at = now;

        ip_addr_t addr;
        const err_t e = dns_gethostbyname("pool.ntp.org", &addr, on_resolved, 0);
        if (e != ERR_OK) return;                   // ERR_INPROGRESS, or no DNS
        server = addr;                             // cached, and answered now
        have_server = true;
    }

    asked_at = now;
    ask();
}
