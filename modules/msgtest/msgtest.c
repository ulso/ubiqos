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

    // --- and the same receive, with a deadline ------------------------------
    // Three cases, because a timeout has three outcomes and only one of them is
    // the timeout. The interesting one is the last: a receiver with a deadline
    // is queued on the sleep list as well as waiting for a message, so a sender
    // arriving first has to take it off both. If it did not, the timer would
    // wake it a second time and receive would return twice.
    myrtos_msg_t m;
    int32_t r;

    r = myrtos_receive_tmo(&m, 0);
    myrtos_write_str(MYRTOS_STDOUT, r == MYRTOS_RECV_TIMEOUT
        ? "  poll with nothing waiting: timed out, as it should\n"
        : "  poll with nothing waiting: WRONG, it returned something\n");

    r = myrtos_receive_tmo(&m, 300);
    myrtos_write_str(MYRTOS_STDOUT, r == MYRTOS_RECV_TIMEOUT
        ? "  300 ms with nobody sending: timed out, as it should\n"
        : "  300 ms with nobody sending: WRONG, it returned something\n");

    child = myrtos_exec("msgcli", "");
    if (child >= 0) {
        r = myrtos_receive_tmo(&m, 5000);
        myrtos_line_reset(&line);
        myrtos_line_str(&line, r >= 0
            ? "  5 s with a sender: got pid "
            : "  5 s with a sender: WRONG, it timed out ");
        myrtos_line_u32(&line, (uint32_t)(r >= 0 ? r : -r));
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
        if (r >= 0) myrtos_reply((int32_t)m.len);

        // The client sends three; take the rest so it can finish.
        for (int i = 0; i < 2; i++) {
            if (myrtos_receive_tmo(&m, 2000) < 0) break;
            myrtos_reply((int32_t)m.len);
        }
        myrtos_wait(child);
    }

    // Linger, so that a second attempt to start this module overlaps with this
    // one. It is marked SINGLE, and the refusal is the thing being shown.
    myrtos_sleep(3000);
    myrtos_write_str(MYRTOS_STDOUT, "msgtest: done\n");
}
