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

#include "../common/myrtos_abi.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

int32_t myrtos_msg_receive_tmo(myrtos_msg_t *out, uint32_t ms);
int32_t myrtos_msg_reply(int32_t status);
int32_t myrtos_net_register(uint32_t stack, int32_t pid);
int32_t myrtos_current_pid(void);

#define NSOCK    8
#define NPENDING 4

typedef struct {
    struct tcp_pcb *pcb;
    struct pbuf    *rx;          // what has arrived and not been read
    int32_t         owner;       // the pid that asked for it
    uint16_t        port;        // non-zero only for a listener
    bool            used;
    bool            gone;        // the peer closed; the data before it stays
    int8_t          pending[NPENDING];
    uint8_t         npending;
} sock_t;

static sock_t sk[NSOCK];

// Where a connection stops. served counts messages the USB task answered at
// all, which separates "httpd never asked" from "the stack never offered".
uint32_t myrtos_lwipsock_served, myrtos_lwipsock_queued, myrtos_lwipsock_taken;
uint32_t myrtos_lwipsock_recv, myrtos_lwipsock_sent;

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
    myrtos_lwipsock_queued++;
    return ERR_OK;
}

// --- THE OPERATIONS -------------------------------------------------------

static int32_t do_listen(uint16_t port, int32_t owner)
{
    int i = alloc_sock();
    if (i < 0) return -1;

    struct tcp_pcb *p = tcp_new();
    if (!p) { free_sock(i); return -1; }
    if (tcp_bind(p, IP_ANY_TYPE, port) != ERR_OK) { tcp_abort(p); free_sock(i); return -1; }

    struct tcp_pcb *l = tcp_listen(p);   // frees p and returns a smaller pcb
    if (!l) { tcp_abort(p); free_sock(i); return -1; }

    sk[i].pcb = l;
    sk[i].port = port;
    sk[i].owner = owner;
    tcp_arg(l, (void *)(intptr_t)i);
    tcp_accept(l, on_accept);
    return MYRTOS_SOCK_MAKE(MYRTOS_NET_LWIP, i);
}

static int32_t do_accept(int i)
{
    if (i < 0 || i >= NSOCK || !sk[i].used || !sk[i].npending) return -1;
    int c = sk[i].pending[0];
    for (int k = 1; k < sk[i].npending; k++) sk[i].pending[k - 1] = sk[i].pending[k];
    sk[i].npending--;
    myrtos_lwipsock_taken++;
    return MYRTOS_SOCK_MAKE(MYRTOS_NET_LWIP, c);
}

// Why a receive said -1, because "it failed" is not a diagnosis: 1 is a bad
// index, 2 is a socket nobody owns, 3 is the peer having closed.
uint32_t myrtos_lwipsock_why;

static int32_t do_recv(int i, uint8_t *buf, uint32_t len)
{
    if (i < 0 || i >= NSOCK)  { myrtos_lwipsock_why = 1; return -1; }
    if (!sk[i].used)          { myrtos_lwipsock_why = 2; return -1; }
    if (!sk[i].rx) {
        if (!sk[i].gone) return 0;                // nothing yet is not an end
        myrtos_lwipsock_why = 3;
        return -1;
    }

    uint16_t n = (uint16_t)(len > 0xffffu ? 0xffffu : len);
    if (n > sk[i].rx->tot_len) n = sk[i].rx->tot_len;
    pbuf_copy_partial(sk[i].rx, buf, n, 0);
    sk[i].rx = pbuf_free_header(sk[i].rx, n);     // NULL once it is all read

    // The window opens again only for what has actually been taken.
    if (sk[i].pcb) tcp_recved(sk[i].pcb, n);
    myrtos_lwipsock_recv += n;
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
    myrtos_lwipsock_sent += n;
    return (int32_t)n;
}

static int32_t do_close(int i)
{
    if (i < 0 || i >= NSOCK || !sk[i].used) return -1;
    if (sk[i].pcb) {
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
    if (i < 0 || i >= NSOCK || !sk[i].used) return MYRTOS_TCP_CLOSED;
    if (!sk[i].pcb) return MYRTOS_TCP_CLOSED;
    return (int32_t)sk[i].pcb->state;
}

// --- THE SERVER -----------------------------------------------------------

int32_t myrtos_lwip_sock_handle(const myrtos_wifi_sock_t *r, int32_t from)
{
    int i = (int)r->arg;
    switch (r->op) {
    case MYRTOS_SOCK_LISTEN: return do_listen((uint16_t)r->arg, from);
    case MYRTOS_SOCK_ACCEPT: return do_accept(i);
    case MYRTOS_SOCK_RECV:   return do_recv(i, r->buf, r->len);
    case MYRTOS_SOCK_SEND:   return do_send(i, r->buf, r->len);
    case MYRTOS_SOCK_CLOSE:  return do_close(i);
    case MYRTOS_SOCK_STATE:  return do_state(i);
    case MYRTOS_SOCK_OWNER:
        return (i >= 0 && i < NSOCK && sk[i].used) ? sk[i].owner : -1;
    case MYRTOS_SOCK_PORT:
        return (i >= 0 && i < NSOCK && sk[i].used) ? sk[i].port : 0;
    default: return -1;
    }
}

// Called from the USB device task's loop, once per turn. Zero milliseconds, so
// a turn with nothing waiting costs one look.
void myrtos_lwip_serve(void)
{
    myrtos_msg_t m;
    int32_t from = myrtos_msg_receive_tmo(&m, 0);
    if (from < 0) return;
    myrtos_lwipsock_served++;
    if (m.type != MYRTOS_MSG_WIFI_SOCK) { myrtos_msg_reply(-1); return; }
    myrtos_msg_reply(myrtos_lwip_sock_handle((const myrtos_wifi_sock_t *)m.data, from));
}

void myrtos_lwip_sock_init(void)
{
    myrtos_net_register(MYRTOS_NET_LWIP, myrtos_current_pid());
}
