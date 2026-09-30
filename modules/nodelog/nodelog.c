#include "../../common/ubiqos_abi.h"

// nodelog -- the log of a node: it receives what the node's other programs
// send it and writes each on the console, with the time it arrived.
//
// Half of the smallest example of what a node is for. A node has no shell and
// starts nothing after boot; its programs are the ones in its module image,
// started in the image's order, and they find each other by name. This one is
// first, so it is already waiting when the others look for it. See
// kernel/node.c, and nodebeat for the other half.
UBIQOS_MEM_SIZE(2048);

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_line_t line;

    ubiqos_write_str(UBIQOS_STDOUT, "nodelog: waiting\n");
    for (;;) {
        ubiqos_msg_t m;
        const int32_t from = ubiqos_receive(&m);
        if (from < 0) continue;

        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "[");
        ubiqos_line_u32(&line, ubiqos_ticks_now());
        ubiqos_line_str(&line, " ms] pid ");
        ubiqos_line_u32(&line, (uint32_t)from);
        ubiqos_line_str(&line, ": ");
        if (from) ubiqos_line_chars(&line, (const char *)m.data, m.len);
        else      ubiqos_line_str(&line, "a pulse");
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);

        // The sender is stopped inside send until this, so its text was safe
        // to read until now. A pulse has nobody waiting and is not answered.
        if (from) ubiqos_reply(0);
    }
}
