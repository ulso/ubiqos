// Sockets on lwIP, answered from the one context lwIP is allowed to have.
//
// The socket dispatcher in syscalls.c turns every call into a message to the
// server that owns the stack. For NINA that is a process of its own, because
// it waits on SPI. For lwIP it cannot be: NO_SYS is 1, so the stack must only
// ever be touched from the USB device task -- which means the "server" is that
// task, taking messages with a zero timeout in the middle of its loop.
//
// Nothing here blocks. tcp_write and tcp_close return immediately, accept and
// receive are answered from what the callbacks have already put aside, and a
// caller that gets -1 asks again -- which is the same contract the NINA side
// has always had.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../common/ubiqos_abi.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/dns.h"

int32_t ubiqos_msg_receive_tmo(ubiqos_msg_t *out, uint32_t ms);
int32_t ubiqos_msg_reply(int32_t status);
int32_t ubiqos_net_register(uint32_t stack, int32_t pid);
int32_t ubiqos_current_pid(void);

#define NSOCK    8
#define NPENDING 4

typedef struct {
    struct tcp_pcb *pcb;
    struct pbuf    *rx;          // what has arrived and not been read
    int32_t         owner;       // the pid that asked for it
    uint16_t        port;        // non-zero only for a listener
    bool            used;
    bool            gone;        // the peer closed; the data before it stays
    bool            connecting;  // ours, on its way out: resolving or handshaking
    uint16_t        want_port;   // where it is going, until there is a pcb
    int8_t          pending[NPENDING];
    uint8_t         npending;
} sock_t;

static sock_t sk[NSOCK];

// Where a connection stops. served counts messages the USB task answered at
// all, which separates "httpd never asked" from "the stack never offered".
uint32_t ubiqos_lwipsock_served, ubiqos_lwipsock_queued, ubiqos_lwipsock_taken;
uint32_t ubiqos_lwipsock_recv, ubiqos_lwipsock_sent;

uint32_t ubiqos_lwipsock_why;
uint32_t ubiqos_lwipsock_lastop, ubiqos_lwipsock_lastreply;

// Callbacks lwIP actually made, which is the one thing not yet measured.
uint32_t ubiqos_lwipsock_oncalls, ubiqos_lwipsock_onbytes;

static int alloc_sock(void)
{
    for (int i = 0; i < NSOCK; i++)
        if (!sk[i].used) { memset(&sk[i], 0, sizeof sk[i]); sk[i].used = true; return i; }
    return -1;
}

static void free_sock(int i)
{
    if (sk[i].rx) { pbuf_free(sk[i].rx); sk[i].rx = NULL; }
    sk[i].used = false;
    sk[i].pcb = NULL;
}

// --- WHAT LWIP CALLS BACK -------------------------------------------------

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    int i = (int)(intptr_t)arg;
    if (err != ERR_OK) { if (p) pbuf_free(p); return err; }

    if (!p) {                      // the peer closed
        sk[i].gone = true;
        return ERR_OK;
    }
    // Kept, not copied: a chain costs nothing to hold and the reader takes it
    // apart at its own pace. Acknowledging happens as it is read, so the window
    // closes when this end falls behind, which is what a window is for.
    ubiqos_lwipsock_oncalls++;
    ubiqos_lwipsock_onbytes += p->tot_len;
    if (sk[i].rx) pbuf_cat(sk[i].rx, p);
    else          sk[i].rx = p;
    (void)pcb;
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    int i = (int)(intptr_t)arg;
    (void)err;
    sk[i].pcb = NULL;              // lwIP has freed it already
    sk[i].gone = true;
    // A connection that failed on its way out ends here too, and clearing this
    // is what stops do_state answering SYN_SENT for ever afterwards.
    sk[i].connecting = false;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    int server = (int)(intptr_t)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    if (sk[server].npending >= NPENDING) return ERR_MEM;   // lwIP will refuse it

    int i = alloc_sock();
    if (i < 0) return ERR_MEM;

    sk[i].pcb = newpcb;
    sk[i].owner = sk[server].owner;
    tcp_arg(newpcb, (void *)(intptr_t)i);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);
    sk[server].pending[sk[server].npending++] = (int8_t)i;
    ubiqos_lwipsock_queued++;
    return ERR_OK;
}

// --- MAKING A CONNECTION --------------------------------------------------
//
// Everything above answers connections somebody else started. This is the other
// direction, and until now the machine had none: a server could be written
// without it, a client cannot. It is the first thing anything like fetch, or an
// HTTP client, or tiny-curl would need.
//
// Non-blocking like the rest of the file, and for the same reason -- lwIP is
// NO_SYS and this is the USB task, which may not wait. The socket exists before
// the connection does, and UBIQOS_SOCK_STATE is how the caller finds out:
// SYN_SENT while it is on its way, ESTABLISHED when it is there, CLOSED if it
// failed.
//
// The name is resolved here rather than by the caller because the resolver is
// lwIP's and lwIP is ours alone. dns_gethostbyname answers straight away for a
// dotted address or a cached name, and otherwise calls back later -- which is
// exactly the case the poll above exists to make bearable.

static void start_connect(int i, const ip_addr_t *addr)
{
    struct tcp_pcb *p = tcp_new();
    if (!p) { sk[i].connecting = false; sk[i].gone = true; ubiqos_lwipsock_why = 32; return; }

    sk[i].pcb = p;
    tcp_arg(p, (void *)(intptr_t)i);
    tcp_recv(p, on_recv);
    tcp_err(p, on_err);

    // on_connected only reports; the state the caller polls is the pcb's own.
    extern err_t ubiqos_sock_on_connected(void *, struct tcp_pcb *, err_t);
    if (tcp_connect(p, addr, sk[i].want_port, ubiqos_sock_on_connected) != ERR_OK) {
        sk[i].connecting = false;
        sk[i].gone = true;
        ubiqos_lwipsock_why = 33;
    }
}

err_t ubiqos_sock_on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    int i = (int)(intptr_t)arg;
    (void)pcb;
    if (i < 0 || i >= NSOCK || !sk[i].used) return ERR_OK;
    sk[i].connecting = false;
    if (err != ERR_OK) { sk[i].gone = true; ubiqos_lwipsock_why = 31; }
    return ERR_OK;
}

// The socket index rides in the callback argument rather than a lookup, so a
// socket closed while its name was still being resolved cannot be mistaken for
// a live one: `used` and `connecting` are both checked before anything happens.
static void on_resolved(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    int i = (int)(intptr_t)arg;
    if (i < 0 || i >= NSOCK || !sk[i].used || !sk[i].connecting) return;
    if (!addr) { sk[i].connecting = false; sk[i].gone = true; ubiqos_lwipsock_why = 30; return; }
    start_connect(i, addr);
}

static int32_t do_connect(const char *host, uint16_t port, int32_t owner)
{
    if (!port || !host[0]) { ubiqos_lwipsock_why = 34; return -1; }

    int i = alloc_sock();
    if (i < 0) { ubiqos_lwipsock_why = 10; return -1; }

    sk[i].owner = owner;
    sk[i].want_port = port;
    sk[i].connecting = true;

    ip_addr_t addr;
    err_t e = dns_gethostbyname(host, &addr, on_resolved, (void *)(intptr_t)i);
    if (e == ERR_OK) {
        start_connect(i, &addr);                  // dotted, or already known
    } else if (e != ERR_INPROGRESS) {
        free_sock(i);
        ubiqos_lwipsock_why = 35;
        return -1;
    }
    return UBIQOS_SOCK_MAKE(UBIQOS_NET_LWIP, i);
}

// --- THE OPERATIONS -------------------------------------------------------

static int32_t do_listen(uint16_t port, int32_t owner)
{
    // Each refusal says which one it was: 10 the table, 11 no pcb, 12 the bind,
    // 13 the listen. "It would not listen" is not a diagnosis.
    int i = alloc_sock();
    if (i < 0) { ubiqos_lwipsock_why = 10; return -1; }

    struct tcp_pcb *p = tcp_new();
    if (!p) { ubiqos_lwipsock_why = 11; free_sock(i); return -1; }

    err_t e = tcp_bind(p, IP_ANY_TYPE, port);
    if (e != ERR_OK) {
        ubiqos_lwipsock_why = 20u + (uint32_t)(-e);   // lwIP's own error, made visible
        tcp_abort(p); free_sock(i); return -1;
    }

    struct tcp_pcb *l = tcp_listen(p);   // frees p and returns a smaller pcb
    if (!l) { ubiqos_lwipsock_why = 13; tcp_abort(p); free_sock(i); return -1; }

    sk[i].pcb = l;
    sk[i].port = port;
    sk[i].owner = owner;
    tcp_arg(l, (void *)(intptr_t)i);
    tcp_accept(l, on_accept);
    return UBIQOS_SOCK_MAKE(UBIQOS_NET_LWIP, i);
}

static int32_t do_accept(int i)
{
    if (i < 0 || i >= NSOCK || !sk[i].used || !sk[i].npending) return -1;
    int c = sk[i].pending[0];
    for (int k = 1; k < sk[i].npending; k++) sk[i].pending[k - 1] = sk[i].pending[k];
    sk[i].npending--;
    ubiqos_lwipsock_taken++;
    return UBIQOS_SOCK_MAKE(UBIQOS_NET_LWIP, c);
}

// Why a receive said -1, because "it failed" is not a diagnosis: 1 is a bad
// index, 2 is a socket nobody owns, 3 is the peer having closed.
static int32_t do_recv(int i, uint8_t *buf, uint32_t len)
{
    if (i < 0 || i >= NSOCK)  { ubiqos_lwipsock_why = 1; return -1; }
    if (!sk[i].used)          { ubiqos_lwipsock_why = 2; return -1; }
    if (!sk[i].rx) {
        if (!sk[i].gone) return 0;                // nothing yet is not an end
        ubiqos_lwipsock_why = 3;
        return -1;
    }

    uint16_t n = (uint16_t)(len > 0xffffu ? 0xffffu : len);
    if (n > sk[i].rx->tot_len) n = sk[i].rx->tot_len;
    pbuf_copy_partial(sk[i].rx, buf, n, 0);
    sk[i].rx = pbuf_free_header(sk[i].rx, n);     // NULL once it is all read

    // The window opens again only for what has actually been taken.
    if (sk[i].pcb) tcp_recved(sk[i].pcb, n);
    ubiqos_lwipsock_recv += n;
    return (int32_t)n;
}

static int32_t do_send(int i, const uint8_t *buf, uint32_t len)
{
    if (i < 0 || i >= NSOCK || !sk[i].used || !sk[i].pcb) return -1;

    uint16_t room = tcp_sndbuf(sk[i].pcb);
    if (!room) return 0;                          // ask again; nothing is lost
    uint16_t n = (uint16_t)(len > room ? room : len);

    // TCP_WRITE_FLAG_COPY, because the caller's buffer is a blocked sender's
    // stack and lwIP keeps what it is given until it is acknowledged.
    if (tcp_write(sk[i].pcb, buf, n, TCP_WRITE_FLAG_COPY) != ERR_OK) return 0;
    tcp_output(sk[i].pcb);
    ubiqos_lwipsock_sent += n;
    return (int32_t)n;
}

static int32_t do_close(int i)
{
    if (i < 0 || i >= NSOCK || !sk[i].used) return -1;
    if (sk[i].pcb && sk[i].pcb->state == LISTEN) {
        // A listening pcb is the smaller tcp_pcb_listen, and lwIP asserts if
        // it is given a recv or err callback -- clearing one included. That
        // assertion is a panic here: the first `kill httpd` took the whole
        // machine with it, from the reaper, because httpd's one socket is a
        // listener. Its only callback is accept, and closing one cannot fail.
        tcp_arg(sk[i].pcb, NULL);
        tcp_accept(sk[i].pcb, NULL);
        tcp_close(sk[i].pcb);
    } else if (sk[i].pcb) {
        tcp_arg(sk[i].pcb, NULL);
        tcp_recv(sk[i].pcb, NULL);
        tcp_err(sk[i].pcb, NULL);
        if (tcp_close(sk[i].pcb) != ERR_OK) tcp_abort(sk[i].pcb);
    }
    free_sock(i);
    return 0;
}

// lwIP's states are the same list in the same order as the ABI's, which is not
// a coincidence -- both come from the TCP state machine in RFC 793.
static int32_t do_state(int i)
{
    if (i < 0 || i >= NSOCK || !sk[i].used) return UBIQOS_TCP_CLOSED;
    // Resolving a name is not a TCP state, but the caller's question is "may I
    // write yet", and the honest answer while a lookup is out is the same as
    // while the handshake is.
    if (sk[i].connecting && !sk[i].pcb) return UBIQOS_TCP_SYN_SENT;
    if (!sk[i].pcb) return UBIQOS_TCP_CLOSED;
    return (int32_t)sk[i].pcb->state;
}

// --- THE SERVER -----------------------------------------------------------

int32_t ubiqos_lwip_sock_handle(const ubiqos_wifi_sock_t *r, int32_t from)
{
    int i = (int)r->arg;
    switch (r->op) {
    case UBIQOS_SOCK_LISTEN: return do_listen((uint16_t)r->arg, from);
    case UBIQOS_SOCK_ACCEPT: return do_accept(i);
    case UBIQOS_SOCK_RECV:   return do_recv(i, r->buf, r->len);
    case UBIQOS_SOCK_SEND:   return do_send(i, r->buf, r->len);
    case UBIQOS_SOCK_CLOSE:  return do_close(i);
    case UBIQOS_SOCK_PEER: {
        if (i < 0 || i >= NSOCK || !sk[i].used || !sk[i].pcb) return -1;
        if (!r->buf || r->len < sizeof(uint32_t)) return -1;
        *(uint32_t *)r->buf = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&sk[i].pcb->remote_ip)));
        return 0;
    }
    case UBIQOS_SOCK_CONNECT: {
        char name[64];
        uint32_t n = r->len > sizeof(name) - 1 ? sizeof(name) - 1 : r->len;
        for (uint32_t k = 0; k < n; k++) name[k] = (char)r->buf[k];
        name[n] = 0;
        return do_connect(name, (uint16_t)r->arg, from);
    }
    // Not a socket, and here for the reason everything else here is: this is
    // the one context lwIP may be touched from.
    case UBIQOS_SOCK_PING: {
        extern int32_t ubiqos_ping_start(const char *host);
        char name[64];
        uint32_t n = r->len > sizeof(name) - 1 ? sizeof(name) - 1 : r->len;
        for (uint32_t k = 0; k < n; k++) name[k] = (char)r->buf[k];
        name[n] = 0;
        return ubiqos_ping_start(name);
    }
    case UBIQOS_SOCK_BROWSE: {
        extern int32_t ubiqos_mdns_browse(const char *service);
        char name[64];
        uint32_t n = r->len > sizeof(name) - 1 ? sizeof(name) - 1 : r->len;
        for (uint32_t k = 0; k < n; k++) name[k] = (char)r->buf[k];
        name[n] = 0;
        return ubiqos_mdns_browse(name);
    }
    case UBIQOS_SOCK_FOUND: {
        extern int32_t ubiqos_mdns_state(uint32_t *addr_out);
        extern uint32_t ubiqos_mdns_found(uint32_t i, char *out, uint32_t cap);
        if ((uint32_t)i == 0xffu) return ubiqos_mdns_state(0);     // still asking?
        return (int32_t)ubiqos_mdns_found((uint32_t)i, (char *)r->buf, r->len);
    }
    case UBIQOS_SOCK_PINGST: {
        extern void ubiqos_ping_poll(uint32_t out[3]);
        if (r->len < 3 * sizeof(uint32_t)) return -1;
        ubiqos_ping_poll((uint32_t *)r->buf);
        return 0;
    }
    case UBIQOS_SOCK_STATE:  return do_state(i);
    case UBIQOS_SOCK_OWNER:
        return (i >= 0 && i < NSOCK && sk[i].used) ? sk[i].owner : -1;   // -2 is reaped
    case UBIQOS_SOCK_PORT:
        return (i >= 0 && i < NSOCK && sk[i].used) ? sk[i].port : 0;
    default: return -1;
    }
}

#define OWNER_DEAD (-2)

// Called from the kernel when a process is reaped. MARKS ONLY, and that is the
// whole point: the reaper runs in kernel context with interrupts off, and
// closing a socket means calling into lwIP, which may only be touched from the
// task below. wifilib says the same thing about SPI, for the same reason.
void ubiqos_lwip_forget_pid(int32_t pid)
{
    for (int i = 0; i < NSOCK; i++)
        if (sk[i].used && sk[i].owner == pid) sk[i].owner = OWNER_DEAD;
}

// Sockets whose owner is gone, closed here because this is the first place
// that may. Without it they simply accumulate: httpd exits without closing,
// and the table filled up until a listen failed -- which read as "no such
// network stack", two attempts out of three.
static void reap(void)
{
    for (int i = 0; i < NSOCK; i++)
        if (sk[i].used && sk[i].owner == OWNER_DEAD) do_close(i);
}

// Called from the USB device task's loop, once per turn. Zero milliseconds, so
// a turn with nothing waiting costs one look.
void ubiqos_lwip_serve(void)
{
    reap();

    ubiqos_msg_t m;
    int32_t from = ubiqos_msg_receive_tmo(&m, 0);
    if (from < 0) return;
    ubiqos_lwipsock_served++;
    if (m.type != UBIQOS_MSG_WIFI_SOCK) {
        ubiqos_lwipsock_why = 99;              // not a socket call at all
        ubiqos_msg_reply(-1);
        return;
    }
    // What was answered, and to which operation, because "httpd saw a refusal"
    // and "the server refused" are different claims and only one of them was
    // ever measured.
    const ubiqos_wifi_sock_t *r = (const ubiqos_wifi_sock_t *)m.data;
    int32_t rc = ubiqos_lwip_sock_handle(r, from);
    ubiqos_lwipsock_lastop = r->op;
    ubiqos_lwipsock_lastreply = (uint32_t)rc;
    ubiqos_msg_reply(rc);
}

void ubiqos_lwip_sock_init(void)
{
    ubiqos_net_register(UBIQOS_NET_LWIP, ubiqos_current_pid());
}
