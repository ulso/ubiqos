#include "../../common/myrtos_abi.h"

MYRTOS_MEM_SIZE(8192);

// ehrpc -- ask the ESP32-C6 a question on the control plane.
//
//   ehrpc mode     which WiFi mode the radio is in
//
// ESP-Hosted carries two things over the SPI. The DATA plane is Ethernet
// frames and belongs to lwIP. This is the other one: scan, connect, what is my
// address -- everything that configures the radio -- and it is remote procedure
// calls with protobuf on the wire.
//
// Espressif's own way in is to take their whole host stack: esp_event,
// esp_netif, their lwIP glue and an OS port layer. That does not fit in sixty
// kilobytes of C heap, so this speaks the wire format instead. The format is
// small enough to be worth it -- the whole of an encoder is below and it is
// forty lines -- because ESP-Hosted's RPC layer is one protobuf message with a
// number in it, and nothing else.
//
//     Rpc {
//         msg_type = 1        Req
//         msg_id   = 2        259 for "which mode"
//         uid      = 3        ours, echoed back
//         <msg_id> = payload  the request itself, at the field number that IS
//                             its message id
//     }
//
// A response comes back with msg_type Resp and msg_id 256 higher than the
// request's, which is the whole of the addressing.

// --- THE WRAPPER ------------------------------------------------------------
//
// The protobuf is not what goes on the wire. It is wrapped in two
// type/length/value pairs, and the length is sixteen bits little-endian:
//
//     [0x01][ep_len:2]["RPCRsp"][0x02][data_len:2][protobuf]
//
// Nothing in the architecture document says so. What said so was the chip: the
// first thing it sent on the control plane after the handshake was an event,
// and reading its twenty bytes -- rather than arguing about the encoder --
// showed 01 06 00 R P C E v t 02 08 00 and eight bytes of protobuf. Espressif's
// own host has the same shape in a comment in eh_host_mcu_vserial.c, which was
// worth finding afterwards and would have been no use before, because the
// question was not answered anywhere the search had been looking.
//
// The endpoint a request goes to is called RPCRsp, which reads backwards and
// is not a mistake: it is the name of the endpoint that deals in responses,
// and the request is what you send there.
#define TLV_EPNAME 0x01u
#define TLV_DATA   0x02u
#define EP_REQ     "RPCRsp"

#define RPC_REQ  1u
#define RPC_RESP 2u

#define REQ_GET_MODE  259u
#define RESP_OFFSET   256u

// --- PROTOBUF, THE THREE PIECES OF IT WE NEED -------------------------------
//
// A field is a varint tag -- the field number shifted up three with the wire
// type in the bottom -- and then either a varint or a length and that many
// bytes. Everything here is one of those two, which is why this is forty lines
// and not a library.

static uint32_t put_varint(uint8_t *p, uint32_t v) {
    uint32_t n = 0;
    while (v >= 0x80u) { p[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}

static uint32_t put_field(uint8_t *p, uint32_t field, uint32_t wire, uint32_t v) {
    uint32_t n = put_varint(p, (field << 3) | wire);
    return n + put_varint(p + n, v);
}

// The value of a varint, and how far to step past it. A malformed one runs off
// the end of the buffer rather than into the next field, which is why the
// limit is passed rather than trusted.
static uint32_t get_varint(const uint8_t *p, uint32_t len, uint32_t *at) {
    uint32_t v = 0, shift = 0;
    while (*at < len) {
        uint8_t b = p[(*at)++];
        v |= (uint32_t)(b & 0x7fu) << shift;
        if (!(b & 0x80u)) break;
        shift += 7;
        if (shift > 28) break;
    }
    return v;
}

// Walk the fields, and hand each one to the caller. Unknown fields are skipped
// by their wire type, which is the property that lets this read a message it
// was not compiled against -- and ESP-Hosted's Rpc has a hundred and forty
// possible payloads, of which we know one.
typedef void (*field_fn)(uint32_t field, uint32_t wire, uint32_t varint,
                         const uint8_t *bytes, uint32_t len, void *arg);

static void walk(const uint8_t *p, uint32_t len, field_fn fn, void *arg) {
    uint32_t at = 0;
    while (at < len) {
        uint32_t tag = get_varint(p, len, &at);
        uint32_t field = tag >> 3, wire = tag & 7u;
        if (!field) return;

        if (wire == 0) {
            uint32_t v = get_varint(p, len, &at);
            fn(field, wire, v, 0, 0, arg);
        } else if (wire == 2) {
            uint32_t n = get_varint(p, len, &at);
            if (at + n > len) return;
            fn(field, wire, 0, p + at, n, arg);
            at += n;
        } else if (wire == 5) { at += 4; }
        else if (wire == 1)   { at += 8; }
        else return;                        // groups: not in this protocol
    }
}

// --- THE ONE QUESTION -------------------------------------------------------

typedef struct {
    uint32_t msg_type, msg_id, uid;
    const uint8_t *payload;
    uint32_t payload_len;
} envelope_t;

static void on_outer(uint32_t field, uint32_t wire, uint32_t v,
                     const uint8_t *b, uint32_t n, void *arg) {
    envelope_t *e = (envelope_t*)arg;
    if (wire == 0) {
        if (field == 1) e->msg_type = v;
        else if (field == 2) e->msg_id = v;
        else if (field == 3) e->uid = v;
    } else if (wire == 2 && field >= 256u) {
        // The payload sits at the field number that IS its message id, so
        // finding it needs no table.
        e->payload = b;
        e->payload_len = n;
    }
}

typedef struct { uint32_t mode, resp, seen; } mode_t_;

static void on_mode(uint32_t field, uint32_t wire, uint32_t v,
                    const uint8_t *b, uint32_t n, void *arg) {
    (void)b; (void)n;
    mode_t_ *m = (mode_t_*)arg;
    if (wire != 0) return;
    if (field == 1) { m->mode = v; m->seen |= 1u; }
    if (field == 2) { m->resp = v; m->seen |= 2u; }
}

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static uint32_t wrap(uint8_t *out, const uint8_t *body, uint32_t body_len) {
    static const char ep[] = EP_REQ;
    uint32_t eplen = sizeof(ep) - 1, n = 0;

    out[n++] = TLV_EPNAME;
    out[n++] = (uint8_t)eplen;
    out[n++] = (uint8_t)(eplen >> 8);
    for (uint32_t i = 0; i < eplen; i++) out[n++] = (uint8_t)ep[i];

    out[n++] = TLV_DATA;
    out[n++] = (uint8_t)body_len;
    out[n++] = (uint8_t)(body_len >> 8);
    for (uint32_t i = 0; i < body_len; i++) out[n++] = body[i];
    return n;
}

// Past the wrapper to the protobuf. The endpoint name is not checked, for the
// same reason Espressif's own host does not check it: what routes a message is
// the id inside it.
static const uint8_t *unwrap(const uint8_t *in, uint32_t len, uint32_t *out_len) {
    uint32_t p = 0;
    if (len < 3 || in[p++] != TLV_EPNAME) return 0;
    uint32_t eplen = (uint32_t)in[p] | ((uint32_t)in[p + 1] << 8);
    p += 2;
    if (p + eplen + 3u > len) return 0;
    p += eplen;

    if (in[p++] != TLV_DATA) return 0;
    uint32_t dlen = (uint32_t)in[p] | ((uint32_t)in[p + 1] << 8);
    p += 2;
    if (p + dlen > len) return 0;

    *out_len = dlen;
    return in + p;
}

static void do_mode(int32_t dev) {
    uint8_t body[32];
    uint32_t n = 0;
    n += put_field(body + n, 1, 0, RPC_REQ);
    n += put_field(body + n, 2, 0, REQ_GET_MODE);
    n += put_field(body + n, 3, 0, 1);                 // uid, echoed back
    n += put_field(body + n, REQ_GET_MODE, 2, 0);      // an empty request body

    uint8_t req[64];
    n = wrap(req, body, n);

    if (myrtos_write(dev, req, n) < 0) {
        say("ehrpc: the transport would not take it\r\n");
        return;
    }

    // The answer comes back on the next exchange or the one after: the
    // co-processor has to be asked before it can answer, and asking is a
    // transaction of its own.
    // Asked before it is read, and never read blind. A read of a device with
    // nothing in it WAITS -- the driver answers readable now, so the scheduler
    // parks the caller until it does -- and waiting for an answer that is not
    // coming is a process nobody can get back. espflash reads /dev/esp the
    // same way and for the same reason.
    uint8_t rsp[256];
    int32_t got = 0;
    for (int wait = 0; wait < 200 && got <= 0; wait++) {
        if (myrtos_readable(dev) > 0) got = myrtos_read(dev, rsp, sizeof(rsp));
        else myrtos_sleep(5);
    }
    if (got <= 0) {
        say("ehrpc: no answer in a second\r\n");
        return;
    }

    uint32_t blen = 0;
    const uint8_t *b = unwrap(rsp, (uint32_t)got, &blen);
    if (!b) { say("ehrpc: that was not a control-plane message\r\n"); return; }

    envelope_t e = { 0, 0, 0, 0, 0 };
    walk(b, blen, on_outer, &e);

    if (e.msg_type != RPC_RESP || e.msg_id != REQ_GET_MODE + RESP_OFFSET) {
        myrtos_line_t l;
        myrtos_line_reset(&l);
        myrtos_line_str(&l, "ehrpc: that was not the answer -- type ");
        myrtos_line_u32(&l, e.msg_type);
        myrtos_line_str(&l, ", id ");
        myrtos_line_u32(&l, e.msg_id);
        myrtos_line_str(&l, "\r\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
        return;
    }

    mode_t_ m = { 0, 0, 0 };
    if (e.payload) walk(e.payload, e.payload_len, on_mode, &m);

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "mode ");
    myrtos_line_u32(&l, m.mode);
    myrtos_line_str(&l, "  (");
    myrtos_line_str(&l, m.mode == 0 ? "off" : m.mode == 1 ? "station"
                      : m.mode == 2 ? "access point" : m.mode == 3 ? "both"
                      : "something else");
    myrtos_line_str(&l, "),  the chip answered ");
    myrtos_line_u32(&l, m.resp);
    // 0x3000 is where the WiFi driver's errors start, and 0x3001 is the first
    // of them. Worth naming: it is the answer a radio gives when nothing has
    // called WifiInit yet, which means the round trip worked perfectly and the
    // question was simply early.
    if (m.resp == 0x3001u) myrtos_line_str(&l, " -- the radio is not initialised");
    else if (m.resp == 0)  myrtos_line_str(&l, " -- no error");
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

// Whatever the chip has said on the control plane and nobody has taken. The
// difference between reading the answer and arguing about the encoder.
static void do_peek(int32_t dev) {
    if (myrtos_readable(dev) <= 0) {
        say("nothing waiting on the control plane\r\n");
        return;
    }
    uint8_t rsp[256];
    int32_t got = myrtos_read(dev, rsp, sizeof(rsp));
    if (got <= 0) { say("it went away between asking and reading\r\n"); return; }

    uint32_t blen = 0;
    const uint8_t *b = unwrap(rsp, (uint32_t)got, &blen);
    envelope_t e = { 0, 0, 0, 0, 0 };
    if (b) walk(b, blen, on_outer, &e);

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_u32(&l, (uint32_t)got);
    myrtos_line_str(&l, " bytes:  type ");
    myrtos_line_u32(&l, e.msg_type);
    myrtos_line_str(&l, ", id ");
    myrtos_line_u32(&l, e.msg_id);
    myrtos_line_str(&l, ", uid ");
    myrtos_line_u32(&l, e.uid);
    myrtos_line_str(&l, ", payload ");
    myrtos_line_u32(&l, e.payload_len);
    myrtos_line_str(&l, "\r\n ");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    myrtos_line_reset(&l);
    for (int32_t i = 0; i < got; i++) {
        myrtos_line_str(&l, " ");
        myrtos_line_hex_byte(&l, rsp[i]);
        if ((i % 16) == 15) {
            myrtos_line_str(&l, "\r\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            myrtos_line_reset(&l);
        }
    }
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: ehrpc mode | peek\n\nAsks the ESP32-C6 a question on ESP-Hosted's "
            "control plane.\n\n  mode    which WiFi mode the radio is in\n"
            "  peek    whatever the chip has said that nobody has taken\n")) return;

    bool mode = argc == 2 && is(argv[1], "mode");
    bool peek = argc == 2 && is(argv[1], "peek");
    if (!mode && !peek) {
        say("usage: ehrpc mode | peek\r\n");
        return;
    }

    int32_t dev = myrtos_open("/dev/eh");
    if (dev < 0) { say("ehrpc: no /dev/eh\r\n"); return; }
    if (mode) do_mode(dev); else do_peek(dev);
    myrtos_close(dev);
}
