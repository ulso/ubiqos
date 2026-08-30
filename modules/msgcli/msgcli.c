#include "../../common/myrtos_abi.h"

// msgcli -- the client half. Sends three messages to msgtest and reports what
// each reply said. Every send blocks until the server has answered, so the
// buffer on this stack is safe to reuse afterwards and not before.
void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    myrtos_line_t line;

    int32_t srv = myrtos_pidof("msgtest");
    if (srv < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "msgcli: no server\n");
        return;
    }

    char buf[32];
    for (int i = 0; i < 3; i++) {
        uint32_t n = 0;
        const char *word = "message ";
        while (word[n]) { buf[n] = word[n]; n++; }
        buf[n++] = (char)('1' + i);

        myrtos_msg_t m;
        m.type = MYRTOS_MSG_WRITE;
        m.len  = n;
        m.data = buf;

        int32_t status = myrtos_send(srv, &m);

        myrtos_line_reset(&line);
        myrtos_line_str(&line, "msgcli: reply ");
        myrtos_line_u32(&line, (uint32_t)status);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
    }
}
