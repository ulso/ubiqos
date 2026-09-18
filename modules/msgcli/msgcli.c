#include "../../common/ubiqos_abi.h"

// msgcli -- the client half. Sends three messages to msgtest and reports what
// each reply said. Every send blocks until the server has answered, so the
// buffer on this stack is safe to reuse afterwards and not before.
void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_line_t line;

    int32_t srv = ubiqos_pidof("msgtest");
    if (srv < 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "msgcli: no server\n");
        return;
    }

    char buf[32];
    for (int i = 0; i < 3; i++) {
        uint32_t n = 0;
        const char *word = "message ";
        while (word[n]) { buf[n] = word[n]; n++; }
        buf[n++] = (char)('1' + i);

        ubiqos_msg_t m;
        m.type = UBIQOS_MSG_WRITE;
        m.len  = n;
        m.data = buf;

        int32_t status = ubiqos_send(srv, &m);

        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "msgcli: reply ");
        ubiqos_line_u32(&line, (uint32_t)status);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
    }

    // And a pulse, which is the other half of the pair: no pointer, no reply,
    // and it does not block. The server sees it as a message from nobody --
    // receive gives it 0 rather than a pid -- and must not answer it.
    int32_t rc = ubiqos_pulse(srv, 99, 0xbeef);
    ubiqos_line_reset(&line);
    ubiqos_line_str(&line, "msgcli: pulse sent, returned ");
    ubiqos_line_u32(&line, (uint32_t)rc);
    ubiqos_line_str(&line, " (and did not wait)\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);
}
