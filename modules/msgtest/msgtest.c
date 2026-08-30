#include "../../common/myrtos_abi.h"

// msgtest -- the server half of the message test. Starts msgcli, then receives
// what it sends and answers each one.
//
// The point being demonstrated is that the string the client sends is never
// copied. It lives in the client's own memory and stays valid because the client
// is stopped inside send until the reply -- so the server may read it right up
// until it answers, and not one byte afterwards.
void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    myrtos_line_t line;

    myrtos_line_reset(&line);
    myrtos_line_str(&line, "msgtest: server is pid ");
    myrtos_line_u32(&line, (uint32_t)myrtos_pidof("msgtest"));
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);

    int32_t child = myrtos_exec("msgcli", "");
    if (child < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "msgtest: cannot start msgcli\n");
        return;
    }

    for (int i = 0; i < 3; i++) {
        myrtos_msg_t m;
        int32_t from = myrtos_receive(&m);
        if (from < 0) {
            myrtos_write_str(MYRTOS_STDOUT, "msgtest: receive failed\n");
            break;
        }
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  from pid ");
        myrtos_line_u32(&line, (uint32_t)from);
        myrtos_line_str(&line, " type ");
        myrtos_line_u32(&line, m.type);
        myrtos_line_str(&line, " len ");
        myrtos_line_u32(&line, m.len);
        myrtos_line_str(&line, ": ");
        myrtos_line_chars(&line, (const char*)m.data, m.len);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);

        myrtos_reply((int32_t)m.len);      // only now may the client touch it
    }

    myrtos_wait(child);
    myrtos_write_str(MYRTOS_STDOUT, "msgtest: done\n");
}
