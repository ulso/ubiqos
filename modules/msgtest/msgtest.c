#include "../../common/ubiqos_abi.h"

// msgtest -- the server half of the message test. Starts msgcli, then receives
// what it sends and answers each one.
//
// The point being demonstrated is that the string the client sends is never
// copied. It lives in the client's own memory and stays valid because the client
// is stopped inside send until the reply -- so the server may read it right up
// until it answers, and not one byte afterwards.
void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_line_t line;

    ubiqos_line_reset(&line);
    ubiqos_line_str(&line, "msgtest: server is pid ");
    ubiqos_line_u32(&line, (uint32_t)ubiqos_pidof("msgtest"));
    ubiqos_line_str(&line, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);

    int32_t child = ubiqos_exec("msgcli", "");
    if (child < 0) {
        ubiqos_write_str(UBIQOS_STDOUT, "msgtest: cannot start msgcli\n");
        return;
    }

    for (int i = 0; i < 3; i++) {
        ubiqos_msg_t m;
        int32_t from = ubiqos_receive(&m);
        if (from < 0) {
            ubiqos_write_str(UBIQOS_STDOUT, "msgtest: receive failed\n");
            break;
        }
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "  from pid ");
        ubiqos_line_u32(&line, (uint32_t)from);
        ubiqos_line_str(&line, " type ");
        ubiqos_line_u32(&line, m.type);
        ubiqos_line_str(&line, " len ");
        ubiqos_line_u32(&line, m.len);
        ubiqos_line_str(&line, ": ");
        ubiqos_line_chars(&line, (const char*)m.data, m.len);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);

        ubiqos_reply((int32_t)m.len);      // only now may the client touch it
    }

    // The pulse the client sends last. receive returns 0 for it, not a pid,
    // which is how a receiver tells a pulse from a message: a message has a
    // sender standing blocked behind it and must be answered, a pulse has
    // nobody and must not be. Replying to 0 fails, so getting it wrong is
    // caught rather than silently leaving a process asleep for ever.
    {
        ubiqos_msg_t pm;
        int32_t from = ubiqos_receive(&pm);
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, from == 0 ? "  a pulse: type " : "  NOT a pulse: type ");
        ubiqos_line_u32(&line, pm.type);
        ubiqos_line_str(&line, " value ");
        ubiqos_line_u32(&line, pm.len);
        ubiqos_line_str(&line, " from pid ");
        ubiqos_line_u32(&line, (uint32_t)pm.sender);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
    }

    ubiqos_wait(child);

    // --- and the same receive, with a deadline ------------------------------
    // Three cases, because a timeout has three outcomes and only one of them is
    // the timeout. The interesting one is the last: a receiver with a deadline
    // is queued on the sleep list as well as waiting for a message, so a sender
    // arriving first has to take it off both. If it did not, the timer would
    // wake it a second time and receive would return twice.
    ubiqos_msg_t m;
    int32_t r;

    r = ubiqos_receive_tmo(&m, 0);
    ubiqos_write_str(UBIQOS_STDOUT, r == UBIQOS_RECV_TIMEOUT
        ? "  poll with nothing waiting: timed out, as it should\n"
        : "  poll with nothing waiting: WRONG, it returned something\n");

    r = ubiqos_receive_tmo(&m, 300);
    ubiqos_write_str(UBIQOS_STDOUT, r == UBIQOS_RECV_TIMEOUT
        ? "  300 ms with nobody sending: timed out, as it should\n"
        : "  300 ms with nobody sending: WRONG, it returned something\n");

    child = ubiqos_exec("msgcli", "");
    if (child >= 0) {
        r = ubiqos_receive_tmo(&m, 5000);
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, r >= 0
            ? "  5 s with a sender: got pid "
            : "  5 s with a sender: WRONG, it timed out ");
        ubiqos_line_u32(&line, (uint32_t)(r >= 0 ? r : -r));
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
        if (r >= 0) ubiqos_reply((int32_t)m.len);

        // The client sends three; take the rest so it can finish.
        for (int i = 0; i < 2; i++) {
            if (ubiqos_receive_tmo(&m, 2000) < 0) break;
            ubiqos_reply((int32_t)m.len);
        }
        ubiqos_wait(child);
    }

    // Linger, so that a second attempt to start this module overlaps with this
    // one. It is marked SINGLE, and the refusal is the thing being shown.
    ubiqos_sleep(3000);
    ubiqos_write_str(UBIQOS_STDOUT, "msgtest: done\n");
}
