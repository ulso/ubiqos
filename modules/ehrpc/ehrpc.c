#include "../../common/myrtos_abi.h"

MYRTOS_MEM_SIZE(16384);

// ehrpc -- ask the ESP32-C6 a question on the control plane.
//
//   ehrpc mode          which WiFi mode the radio is in
//   ehrpc peek          what the chip has said that nobody has taken
//   ehrpc connect SSID  join a network, asking for the password here
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
#define REQ_GET_PS    271u
#define REQ_GET_RSSI  341u
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

// A length-delimited field: the tag, the length, and the bytes.
static uint32_t put_bytes(uint8_t *p, uint32_t field, const uint8_t *b, uint32_t len) {
    uint32_t n = put_varint(p, (field << 3) | 2u);
    n += put_varint(p + n, len);
    for (uint32_t i = 0; i < len; i++) p[n + i] = b[i];
    return n + len;
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

// One request out, one response in, and the response's own "resp" field back.
// Every one of these RPCs answers with an int32 at field 1 of its payload,
// which is the esp_err_t the real call returned on the far side -- so a step
// that fails says why in the chip's own numbering rather than ours.
typedef struct { uint32_t resp, seen; } result_t;

static void on_result(uint32_t field, uint32_t wire, uint32_t v,
                      const uint8_t *b, uint32_t n, void *arg) {
    (void)b; (void)n;
    result_t *r = (result_t*)arg;
    if (wire == 0 && field == 1) { r->resp = v; r->seen = 1; }
}

static const char *err_name(uint32_t e) {
    switch (e) {
    case 0:      return "ok";
    case 0x3001: return "the radio is not initialised";
    case 0x3002: return "the radio is not started";
    case 0x3003: return "the radio is not stopped";
    case 0x3004: return "interface error";
    case 0x300a: return "no memory";
    case 0x300b: return "not connected";
    case 0x102:  return "invalid argument";
    default:     return 0;
    }
}

static uint32_t next_uid = 1;

// Send one request and wait for its answer. Returns the chip's esp_err_t, or
// 0xffffffff when nothing came back at all -- which is a different failure and
// worth telling apart from one the chip reported.
static uint32_t events_seen;

static uint32_t call(int32_t dev, uint32_t msg_id,
                     const uint8_t *body, uint32_t body_len, uint32_t ms,
                     uint8_t *out_payload, uint32_t out_cap, uint32_t *out_len) {
    // Anything already waiting is not the answer to a question not yet asked.
    // Left there, a late reply to the LAST request is what the next one reads,
    // and every answer after that is one behind -- which is exactly what
    // happened the first time: WifiInit answered after its two seconds were
    // up, and the next command reported the mode as "type 2, id 534".
    uint8_t drop[512];
    while (myrtos_readable(dev) > 0) {
        if (myrtos_read(dev, drop, sizeof(drop)) <= 0) break;
    }

    uint8_t inner[256];
    uint32_t n = 0;
    n += put_field(inner + n, 1, 0, RPC_REQ);
    n += put_field(inner + n, 2, 0, msg_id);
    n += put_field(inner + n, 3, 0, next_uid++);
    n += put_bytes(inner + n, msg_id, body, body_len);

    uint8_t req[320];
    uint32_t wn = wrap(req, inner, n);

    if (myrtos_write(dev, req, wn) < 0) return 0xfffffffeu;

    // Keep reading until the answer to THIS question arrives.
    //
    // The chip pushes events on the same interface -- the radio started, a
    // station connected, a scan finished -- and they arrive whenever they
    // happen, which is in the middle of a request as often as not. Taking one
    // frame and judging it was enough while the link was silent, and stopped
    // being enough the moment the radio was doing something.
    uint8_t rsp[512];
    for (uint32_t waited = 0; waited < ms; waited += 5) {
        if (myrtos_readable(dev) <= 0) { myrtos_sleep(5); continue; }

        int32_t got = myrtos_read(dev, rsp, sizeof(rsp));
        if (got <= 0) continue;

        uint32_t blen = 0;
        const uint8_t *b = unwrap(rsp, (uint32_t)got, &blen);
        if (!b) continue;

        envelope_t e = { 0, 0, 0, 0, 0 };
        walk(b, blen, on_outer, &e);
        if (e.msg_type != RPC_RESP || e.msg_id != msg_id + RESP_OFFSET) {
            events_seen++;                      // an event, or somebody else's
            continue;
        }

        if (out_payload && e.payload) {
            uint32_t n = e.payload_len > out_cap ? out_cap : e.payload_len;
            for (uint32_t i = 0; i < n; i++) out_payload[i] = e.payload[i];
            *out_len = n;
        }

        result_t r = { 0, 0 };
        if (e.payload) walk(e.payload, e.payload_len, on_result, &r);
        return r.resp;
    }
    return 0xffffffffu;
}

static void do_mode(int32_t dev) {
    uint8_t payload[64];
    uint32_t plen = 0;
    uint32_t resp = call(dev, REQ_GET_MODE, 0, 0, 3000, payload, sizeof(payload), &plen);

    if (resp == 0xffffffffu) { say("ehrpc: no answer\r\n"); return; }
    if (resp == 0xfffffffeu) { say("ehrpc: the transport would not take it\r\n"); return; }

    mode_t_ m = { 0, 0, 0 };
    walk(payload, plen, on_mode, &m);

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "mode ");
    myrtos_line_u32(&l, m.mode);
    myrtos_line_str(&l, "  (");
    myrtos_line_str(&l, m.mode == 0 ? "off" : m.mode == 1 ? "station"
                      : m.mode == 2 ? "access point" : m.mode == 3 ? "both"
                      : "something else");
    // m.resp, not the value call() returned. call() reads field 1 as the
    // result because that is where every ACTION puts it -- WifiInit, SetMode,
    // Start, Connect all answer int32 resp = 1 -- but a getter puts the value
    // it was asked for there and its result at field 2. Printing call()'s
    // answer here said "the chip answered 1" when 1 was the mode.
    myrtos_line_str(&l, "),  the chip answered ");
    myrtos_line_u32(&l, m.resp);
    const char *name = err_name(m.resp);
    if (name) { myrtos_line_str(&l, " -- "); myrtos_line_str(&l, name); }
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

// Bring the radio up and join a network.
//
// THE PASSWORD IS TYPED HERE AND NOWHERE ELSE. Not an argument -- argv lives in
// this process's memory and the shell keeps sixteen lines of history -- not
// echoed, and wiped before this returns. It is the same rule `wifi connect` has
// always had, and it survives the change of radio.
//
// What does NOT work yet is taking it from /sd/config.txt. The kernel holds
// those bytes and will not hand them to a process, which is the whole point of
// how that file is treated: the NINA path got round it by having the kernel do
// the joining. The same will have to happen here -- the message built where the
// password already is -- and that is a change to the driver rather than to this.
// Everything that needs no secret: the radio initialised, put in station mode
// and started. Separate because it is the half that can be tested from a
// serial session, and because a scan will want exactly this and no password.
// Join a network, by asking the driver to do it.
//
// The sequence itself is not here any more. It moved into the driver so that
// /sd/config.txt could supply the password -- the kernel reads that file and
// hands the bytes to the driver and to nothing else -- and once it had moved
// there was no reason to keep a second copy for the typed case. What is left
// here is the typing.
//
// THE PASSWORD IS TYPED AND NOWHERE ELSE. Not an argument -- argv lives in this
// process's memory and the shell keeps sixteen lines of history -- not echoed,
// and wiped before this returns.
static void do_connect(int32_t dev, const char *ssid) {
    char creds[100];
    uint32_t n = 0;
    while (ssid[n] && n < 32) { creds[n] = ssid[n]; n++; }
    creds[n++] = 0;
    uint32_t pass_at = n;

    myrtos_write_str(MYRTOS_STDOUT, "password: ");
    for (;;) {
        uint8_t ch;
        if (myrtos_read(MYRTOS_STDIN, &ch, 1) <= 0) continue;
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3) { n = pass_at; break; }              // ctrl-C: forget it
        if (ch == 8 || ch == 127) { if (n > pass_at) n--; continue; }
        // Not echoed, and not drawn. What is typed here should not survive on
        // the screen, in a scrollback, or in anybody's terminal capture.
        if (ch >= ' ' && n < sizeof(creds) - 2) creds[n++] = (char)ch;
    }
    creds[n++] = 0;
    myrtos_write_str(MYRTOS_STDOUT, "\r\n");

    if (n == pass_at + 1) { say("nothing typed\r\n"); return; }

    int32_t r = myrtos_setstat(dev, MYRTOS_SS_EH_JOIN, creds, n);
    for (uint32_t i = 0; i < sizeof(creds); i++) creds[i] = 0;
    if (r < 0) { say("the driver would not take it\r\n"); return; }

    say("joining");
    for (int waited = 0; waited < 400; waited++) {
        uint32_t state = 0;
        myrtos_getstat(dev, MYRTOS_SS_EH_JOINED, &state, sizeof(state));
        if (state == 2) { say("\r\njoined\r\n"); return; }
        if (state == 3) { say("\r\nit did not join -- the console log says why\r\n"); return; }
        if ((waited % 20) == 0) say(".");
        myrtos_sleep(100);
    }
    say("\r\nstill trying after forty seconds\r\n");
}

// Which power-saving mode the radio is actually in, as opposed to the one it
// was told to be in. esp_wifi_set_ps answers ok whether or not the setting
// survives what comes after it, so the only way to know is to ask.
//
// Unlike GetMode this one is the ordinary way round -- resp at field 1, the
// value at field 2 -- which is a reminder that the layout is per message and
// not a rule.
typedef struct { uint32_t type, seen; } ps_t;

static void on_ps(uint32_t field, uint32_t wire, uint32_t v,
                  const uint8_t *b, uint32_t n, void *arg) {
    (void)b; (void)n;
    ps_t *m = (ps_t *)arg;
    if (wire == 0 && field == 2) { m->type = v; m->seen = 1; }
}

static void do_ps(int32_t dev) {
    uint8_t payload[64];
    uint32_t plen = 0;
    uint32_t resp = call(dev, REQ_GET_PS, 0, 0, 3000, payload, sizeof(payload), &plen);
    if (resp == 0xffffffffu) { say("ehrpc: no answer\r\n"); return; }

    ps_t m = { 0, 0 };
    walk(payload, plen, on_ps, &m);

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "power save ");
    myrtos_line_u32(&l, m.type);
    myrtos_line_str(&l, "  (");
    myrtos_line_str(&l, m.type == 0 ? "off -- the radio stays awake"
                      : m.type == 1 ? "minimum modem sleep: it wakes on beacons"
                      : m.type == 2 ? "maximum modem sleep" : "something else");
    myrtos_line_str(&l, ")\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

// How strong the signal is, which is the one fact about the radio that no
// amount of measuring on this side can supply -- and the obvious explanation
// for a slow link that had never been checked.
static void do_rssi(int32_t dev) {
    uint8_t payload[64];
    uint32_t plen = 0;
    uint32_t resp = call(dev, REQ_GET_RSSI, 0, 0, 3000, payload, sizeof(payload), &plen);
    if (resp == 0xffffffffu) { say("ehrpc: no answer\r\n"); return; }

    ps_t m = { 0, 0 };                        // the same shape: value at field 2
    walk(payload, plen, on_ps, &m);

    // A negative dBm arrives as a protobuf varint of a signed value, which for
    // a small negative number is a very large unsigned one. Reading it as
    // unsigned prints 4294967230 and means nothing to anybody.
    int32_t dbm = (int32_t)m.type;

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "signal -");
    myrtos_line_u32(&l, (uint32_t)(-dbm));
    myrtos_line_str(&l, " dBm  (");
    myrtos_line_str(&l, dbm > -50 ? "excellent" : dbm > -60 ? "good"
                      : dbm > -70 ? "fair" : dbm > -80 ? "weak" : "very weak");
    myrtos_line_str(&l, ")\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: ehrpc mode | peek | connect <ssid>\n\n"
            "Asks the ESP32-C6 a question on ESP-Hosted's control plane.\n\n"
            "  mode    which WiFi mode the radio is in\n"
            "  ps      which power-saving mode it is actually in\n"
            "  rssi    how strong the signal from the access point is\n"
            "  peek    whatever the chip has said that nobody has taken\n"
            "  connect <ssid>  bring the radio up and join. The password is\n"
            "          typed here, never echoed and never an argument -- and\n"
            "          put it in /sd/config.txt to have the board do this\n"
            "          for itself at boot\n")) return;

    bool mode = argc == 2 && is(argv[1], "mode");
    bool peek = argc == 2 && is(argv[1], "peek");
    bool conn = argc == 3 && is(argv[1], "connect");
    bool ps   = argc == 2 && is(argv[1], "ps");
    bool rssi = argc == 2 && is(argv[1], "rssi");
    if (!mode && !peek && !conn && !ps && !rssi) {
        say("usage: ehrpc mode | ps | rssi | peek | connect <ssid>\r\n");
        return;
    }

    int32_t dev = myrtos_open("/dev/eh");
    if (dev < 0) { say("ehrpc: no /dev/eh\r\n"); return; }
    if (mode)      do_mode(dev);
    else if (ps)   do_ps(dev);
    else if (rssi) do_rssi(dev);
    else if (peek) do_peek(dev);
    else           do_connect(dev, argv[2]);
    myrtos_close(dev);
}
