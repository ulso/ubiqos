#include "../../common/ubiqos_abi.h"

// netcon -- the shell, over TCP.
//
//   netcon              answer on port 23, to the USB cable only
//   netcon 2323         another port
//   netcon -a           answer on every interface, WiFi included
//
// A machine with a network ought to be reachable from one. UbiqOS has had a
// console on a screen, on the USB serial port and over Bluetooth, and none of
// them over the network it has been serving web pages on since August.
//
// THE SHELL IS NOT WRITTEN HERE. This starts the ordinary `sh` with a pipe on
// each side of it and carries bytes between those pipes and the socket. That
// is the whole trick, and it is why this is three hundred lines rather than a
// second console: pipes and dup already existed, because the shell needs them
// for its own redirection, and a process started with a pipe on descriptor 0
// cannot tell it from a serial port.
//
// WHAT IT IS NOT: encrypted, or authenticated. Anybody who can reach the port
// gets a shell, and every keystroke crosses the network in the clear. That is
// why the default is the cable and nothing else -- 192.168.7.0/24 is a wire
// between two machines with no third party on it. `-a` opens the same shell to
// anybody on the WiFi, which is a decision, not a convenience.
//
// This is also the half of SSH that has nothing to do with cryptography, which
// is the other reason it exists first.

#define DEFAULT_PORT   23
#define USB_SUBNET     0xC0A80700u      // 192.168.7.0
#define USB_MASK       0xFFFFFF00u
#define NET_WAIT_S     30
#define BUFSZ          256

UBIQOS_MEM_SIZE(8192);

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

static uint32_t to_u32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void put_addr(ubiqos_line_t *l, uint32_t ip) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        ubiqos_line_u32(l, (ip >> shift) & 0xffu);
        if (shift) ubiqos_line_str(l, ".");
    }
}

// --- TELNET -----------------------------------------------------------------
//
// A telnet client announces itself with IAC (255) and a handful of options,
// and a raw one -- nc, or a socket from a script -- sends nothing at all. So
// the negotiation is ANSWERED, never offered: replying to bytes that arrived
// cannot put line noise on the screen of a client that does not speak telnet.
//
// What is worth negotiating is the other end's line editing. Left alone, a
// telnet client collects a whole line, echoes it itself, and sends it on
// return -- so there is no Ctrl-C, no arrow keys and every character appears
// twice, once from the client and once from the shell. WILL ECHO and WILL
// SUPPRESS-GO-AHEAD together are what turn that off.

#define IAC  255u
#define DONT 254u
#define DO   253u
#define WONT 252u
#define WILL 251u
#define SB   250u
#define SE   240u
#define OPT_ECHO 1u
#define OPT_SGA  3u

static bool telnet_seen;

static void offer_char_mode(int32_t sock) {
    const uint8_t msg[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA };
    ubiqos_sock_send(sock, msg, sizeof msg);
}

// Take the telnet out of what arrived; what is left is what was typed. The
// state carries across calls because a negotiation can be split over two
// packets, which is exactly the sort of thing that works for a month and then
// does not.
static uint32_t strip_telnet(int32_t sock, uint8_t *b, uint32_t n) {
    static uint32_t state;          // 0 data, 1 after IAC, 2 after DO/WILL/..,
    static uint8_t verb;            // 3 inside a subnegotiation, 4 its IAC
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (state) {
        case 0:
            if (c == IAC) { state = 1; break; }
            b[out++] = c;
            break;
        case 1:
            if (!telnet_seen) { telnet_seen = true; offer_char_mode(sock); }
            if (c == IAC) { b[out++] = IAC; state = 0; break; }   // a real 255
            if (c == SB)  { state = 3; break; }
            if (c == DO || c == DONT || c == WILL || c == WONT) { verb = c; state = 2; break; }
            state = 0;                                            // a two-byte command
            break;
        case 2: {
            // Everything is refused except the echo and go-ahead we asked for,
            // and those were already offered above.
            uint8_t answer[3] = { IAC, 0, c };
            if (verb == DO)        answer[1] = (c == OPT_ECHO || c == OPT_SGA) ? WILL : WONT;
            else if (verb == WILL) answer[1] = DONT;
            else                   answer[1] = 0;
            if (answer[1]) ubiqos_sock_send(sock, answer, sizeof answer);
            state = 0;
            break;
        }
        case 3: if (c == IAC) state = 4; break;
        case 4: state = (c == SE) ? 0 : 3; break;
        }
    }
    return out;
}

// --- THE SESSION ------------------------------------------------------------

// A module has no C library, so no memmove -- and a plain loop here is one gcc
// turns back into a call to memmove, which then fails to link. Kept out of
// line so that it stays a loop.
static __attribute__((noinline)) void shift_down(uint8_t *to, const uint8_t *from, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) to[i] = from[i];
}

// A newline on its own moves down but not back: the console's driver turns LF
// into CR LF and a pipe does not, so a shell whose output looks right on the
// screen comes out as a staircase over the network. This is what a pty layer
// does, and it is the whole of what one would be needed for here.
static uint32_t crlf(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t k = 0;
    uint8_t prev = 0;
    for (uint32_t i = 0; i < n && k + 2 <= cap; i++) {
        if (in[i] == '\n' && prev != '\r') out[k++] = '\r';
        out[k++] = in[i];
        prev = in[i];
    }
    return k;
}


static bool child_alive(int32_t pid) {
    for (uint32_t s = 0; s < UBIQOS_PS_SLOTS; s++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(s, &p) != 0) continue;
        if ((int32_t)p.pid == pid) return p.state != UBIQOS_PS_ZOMBIE;
    }
    return false;
}

// One connection, from the shell's birth to its death.
//
// The shell gets a TERMINAL: 0, 1 and 2 all name one end of a two-way pair and
// this program holds the other. Two one-way pipes are not the same thing --
// `more` prints its prompt on descriptor 2 and reads the key from descriptor 2.
static void session(int32_t sock) {
    int32_t pair[2];
    if (ubiqos_pipepair(pair) < 0) return;

    const int32_t s0 = ubiqos_dup(UBIQOS_STDIN, -1);
    const int32_t s1 = ubiqos_dup(UBIQOS_STDOUT, -1);
    const int32_t s2 = ubiqos_dup(UBIQOS_STDERR, -1);
    ubiqos_dup(pair[1], UBIQOS_STDIN);
    ubiqos_dup(pair[1], UBIQOS_STDOUT);
    ubiqos_dup(pair[1], UBIQOS_STDERR);
    const int32_t pid = ubiqos_exec("sh", "");
    ubiqos_dup(s0, UBIQOS_STDIN);
    ubiqos_dup(s1, UBIQOS_STDOUT);
    ubiqos_dup(s2, UBIQOS_STDERR);
    ubiqos_close(s0);
    ubiqos_close(s1);
    ubiqos_close(s2);
    ubiqos_close(pair[1]);

    if (pid < 0) {
        const char *no = "netcon: no shell on this machine\r\n";
        ubiqos_sock_send(sock, (const uint8_t *)no, 34);
        ubiqos_close(pair[0]);
        return;
    }

    telnet_seen = false;
    uint8_t buf[BUFSZ];
    uint8_t held[BUFSZ];                          // typed, not yet taken
    uint32_t held_n = 0;
    for (;;) {
        bool moved = false;

        // The client's keystrokes into the shell -- as the shell has room, and
        // never by waiting for it. Writing into a busy shell used to block
        // here, and then nothing read the socket: the client's packets piled up
        // in the stack until it had no buffers left for anything, on either
        // interface. Now what does not fit waits in `held`, and while it waits
        // the socket is not read, so TCP's own window holds the client back.
        if (held_n) {
            const int32_t k = ubiqos_write_some(pair[0], held, held_n);
            if (k > 0) {
                shift_down(held, held + k, held_n - (uint32_t)k);
                held_n -= (uint32_t)k;
                moved = true;
            }
        }
        if (!held_n) {
            const int32_t got = ubiqos_sock_recv(sock, buf, sizeof buf);
            if (got < 0) break;                   // the other end has gone
            if (got > 0) {
                const uint32_t n = strip_telnet(sock, buf, (uint32_t)got);
                const int32_t k = n ? ubiqos_write_some(pair[0], buf, n) : 0;
                const uint32_t took = k > 0 ? (uint32_t)k : 0;
                shift_down(held, buf + took, n - took);
                held_n = n - took;
                moved = true;
            }
        }

        // And what the shell has to say back, with its line endings made
        // whole -- see crlf above.
        while (ubiqos_readable(pair[0]) > 0) {
            const int32_t n = ubiqos_read(pair[0], buf, sizeof buf);
            if (n <= 0) break;
            uint8_t wire[BUFSZ * 2];
            const uint32_t k = crlf(buf, (uint32_t)n, wire, sizeof wire);
            if (ubiqos_sock_send(sock, wire, k) < 0) goto done;
            moved = true;
        }

        if (!child_alive(pid)) break;             // somebody typed exit
        if (!moved) ubiqos_sleep(10);             // nothing happening: be cheap
    }

done:
    // Closing the shell's input is how it is asked to leave: an empty pipe
    // with no writer reads as the end of the file, and `sh` treats that as
    // exit. Only if it will not take the hint does it get killed.
    ubiqos_close(pair[0]);
    for (int i = 0; i < 20 && child_alive(pid); i++) ubiqos_sleep(50);
    if (child_alive(pid)) ubiqos_kill(pid);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: netcon [port] [-a]\n\n"
            "A shell over TCP: connect with telnet or nc and get the same\n"
            "prompt as the screen and the serial port have.\n\n"
            "  port   which port to answer on; 23 by default\n"
            "  -a     answer on every interface. Without it only the USB\n"
            "         cable's own subnet, 192.168.7.x, is let in\n\n"
            "Nothing here is encrypted and nothing is asked for a password:\n"
            "whoever reaches the port gets the shell. The cable is a wire\n"
            "between two machines; the WiFi is not. Run it in the background\n"
            "with 'netcon &' and stop it with 'kill netcon'.\n"))
        return;

    uint32_t port = DEFAULT_PORT;
    bool anywhere = false;
    for (int i = 1; i < argc; i++) {
        if (is(argv[i], "-a")) anywhere = true;
        else if (to_u32(argv[i])) port = to_u32(argv[i]);
    }

    // The stack comes up when the card has been read and the network joined,
    // which is later than a program started from /sd/startup.
    int32_t server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    for (uint32_t waited = 0; server < 0 && waited < NET_WAIT_S; waited++) {
        ubiqos_sleep(1000);
        server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    }
    if (server < 0) {
        {
            // The stack knows why and has always known: 10 is a full socket
            // table, 20 and up are lwIP's own bind errors -- 28 is the port
            // being in use. Printing "would not" and stopping there sent two
            // people looking in the wrong place twice.
            const int32_t why = ubiqos_syscall(SYS_MEMINFO, UBIQOS_MEM_SOCKETS, 102, 0);
            ubiqos_line_t l;
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "netcon: the network stack would not take port ");
            ubiqos_line_u32(&l, port);
            ubiqos_line_str(&l, why == 10 ? " -- no free socket" :
                                why == 28 ? " -- something else is on it" : " -- reason ");
            if (why != 10 && why != 28) ubiqos_line_u32(&l, (uint32_t)why);
            ubiqos_line_str(&l, "\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
        }
        return;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "netcon: a shell on port ");
    ubiqos_line_u32(&l, port);
    ubiqos_line_str(&l, anywhere ? ", to anybody who asks\r\n"
                                 : ", to the USB cable only\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    for (;;) {
        const int32_t c = ubiqos_sock_accept(server);
        if (c < 0) { ubiqos_sleep(100); continue; }

        uint32_t ip = 0;
        ubiqos_sock_peer(c, &ip);

        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "netcon: ");
        put_addr(&l, ip);

        if (!anywhere && (ip & USB_MASK) != USB_SUBNET) {
            // Said to them as well as to the log. A refusal that looks like a
            // broken port is worse than one that says what it is.
            const char *no = "netcon: this shell answers on the USB cable only.\r\n"
                             "        Start it with -a to open it to the network.\r\n";
            uint32_t n = 0;
            while (no[n]) n++;
            ubiqos_sock_send(c, (const uint8_t *)no, n);
            ubiqos_sock_close(c);
            ubiqos_line_str(&l, " turned away\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            continue;
        }

        ubiqos_line_str(&l, " connected\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);

        session(c);
        ubiqos_sock_close(c);

        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "netcon: ");
        put_addr(&l, ip);
        ubiqos_line_str(&l, " gone\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}
