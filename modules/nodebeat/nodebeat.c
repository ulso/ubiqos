#include "../../common/ubiqos_abi.h"

// nodebeat -- the other half of the node example: once a second it sends
// nodelog a line saying so, and once, at the start, it asks for a program to
// be started and reports what a node answers to that.
//
// It finds nodelog by name rather than being told its pid. The image starts
// nodelog first, but a program that assumes the order it was started in
// breaks the day the image is reordered, so it looks until it finds it.
UBIQOS_MEM_SIZE(2048);

static void say(int32_t to, const ubiqos_line_t *line) {
    ubiqos_msg_t m;
    m.type = UBIQOS_MSG_WRITE;
    m.len  = line->len;
    m.data = (void *)line->buf;
    ubiqos_send(to, &m);            // blocks until nodelog has written it
}

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;

    int32_t log;
    while ((log = ubiqos_pidof("nodelog")) < 0) ubiqos_sleep(10);

    // Nothing starts on a node after boot. Ask anyway, and say what came back.
    ubiqos_line_t line;
    ubiqos_line_reset(&line);
    ubiqos_line_str(&line, "asked to start uname, and was answered ");
    const int32_t rc = ubiqos_exec("uname", "");
    if (rc < 0) { ubiqos_line_str(&line, "-"); ubiqos_line_u32(&line, (uint32_t)-rc); }
    else        ubiqos_line_u32(&line, (uint32_t)rc);
    ubiqos_line_str(&line, rc == UBIQOS_EXEC_STATIC ? " -- a node starts nothing after boot" : "");
    say(log, &line);

    for (uint32_t beat = 1;; beat++) {
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "beat ");
        ubiqos_line_u32(&line, beat);
        say(log, &line);
        ubiqos_sleep(1000);
    }
}
