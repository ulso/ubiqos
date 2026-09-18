#include "../../common/ubiqos_abi.h"

// armtest -- wait for several sources and a deadline in the same place.
//
// Before arming there was no way to do this. ubiqos_read blocks on one
// descriptor and gives you nothing else; ubiqos_receive_tmo blocks on messages
// and a clock and knows nothing about descriptors. A program that wanted "the
// dongle answered, or a key was pressed, or three seconds passed" had to poll
// with ubiqos_readable and burn its quantum doing it.
//
// Armed, each descriptor sends its own pulse when it has something, and there
// is one waiting place. The pulse's type says which source, and its value is
// the descriptor -- so a program can tell them apart either way.
#define PULSE_CONSOLE 7
#define PULSE_SECOND  8

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_line_t line;

    // A second path to the same console, which is the only second source this
    // can arrange on its own. It stands in for what a real program would arm:
    // /dev/acm for a dongle, /dev/kbd for the board's own keyboard.
    int32_t second = ubiqos_open("/dev/usb");

    ubiqos_line_reset(&line);
    ubiqos_line_str(&line, "armtest: second path is ");
    ubiqos_line_u32(&line, (uint32_t)second);
    ubiqos_line_str(&line, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);

    if (ubiqos_arm(UBIQOS_STDIN, PULSE_CONSOLE) != 0 ||
        (second >= 0 && ubiqos_arm(second, PULSE_SECOND) != 0)) {
        ubiqos_write_str(UBIQOS_STDOUT, "armtest: no room to watch\n");
        return;
    }

    ubiqos_write_str(UBIQOS_STDOUT, "armtest: two sources armed, 5 s...\n");

    // The loop a real program has: one wait, and everything arrives at it.
    for (;;) {
        ubiqos_msg_t m;
        int32_t from = ubiqos_receive_tmo(&m, 5000);

        if (from == UBIQOS_RECV_TIMEOUT) {
            ubiqos_write_str(UBIQOS_STDOUT, "armtest: nothing more; done\n");
            break;
        }
        if (from != 0) {                    // a real message, which we do not expect
            ubiqos_reply(0);
            continue;
        }

        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "  pulse type ");
        ubiqos_line_u32(&line, m.type);
        ubiqos_line_str(&line, m.type == PULSE_CONSOLE ? " (stdin)" : " (second path)");
        ubiqos_line_str(&line, ", descriptor ");
        ubiqos_line_u32(&line, m.len);
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);

        // First one wins: the rest are of no interest now, and each would
        // otherwise fire one stray pulse into a receive that is no longer
        // expecting it. This is what disarm_all is for.
        int32_t dropped = ubiqos_disarm_all();
        ubiqos_line_reset(&line);
        ubiqos_line_str(&line, "  first one wins; dropped ");
        ubiqos_line_u32(&line, (uint32_t)dropped);
        ubiqos_line_str(&line, " (watches and pulses already sent)\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);

        uint8_t ch = 0;
        if (ubiqos_readable((int32_t)m.len) > 0) ubiqos_read((int32_t)m.len, &ch, 1);
    }

    ubiqos_arm(UBIQOS_STDIN, 0);
    if (second >= 0) { ubiqos_arm(second, 0); ubiqos_close(second); }
}
