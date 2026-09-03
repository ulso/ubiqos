#include "../../common/myrtos_abi.h"

// armtest -- wait for several sources and a deadline in the same place.
//
// Before arming there was no way to do this. myrtos_read blocks on one
// descriptor and gives you nothing else; myrtos_receive_tmo blocks on messages
// and a clock and knows nothing about descriptors. A program that wanted "the
// dongle answered, or a key was pressed, or three seconds passed" had to poll
// with myrtos_readable and burn its quantum doing it.
//
// Armed, each descriptor sends its own pulse when it has something, and there
// is one waiting place. The pulse's type says which source, and its value is
// the descriptor -- so a program can tell them apart either way.
#define PULSE_CONSOLE 7
#define PULSE_SECOND  8

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    myrtos_line_t line;

    // A second path to the same console, which is the only second source this
    // can arrange on its own. It stands in for what a real program would arm:
    // /dev/acm for a dongle, /dev/kbd for the board's own keyboard.
    int32_t second = myrtos_open("/dev/usb");

    myrtos_line_reset(&line);
    myrtos_line_str(&line, "armtest: second path is ");
    myrtos_line_u32(&line, (uint32_t)second);
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);

    if (myrtos_arm(MYRTOS_STDIN, PULSE_CONSOLE) != 0 ||
        (second >= 0 && myrtos_arm(second, PULSE_SECOND) != 0)) {
        myrtos_write_str(MYRTOS_STDOUT, "armtest: no room to watch\n");
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "armtest: two sources armed, 5 s...\n");

    // The loop a real program has: one wait, and everything arrives at it.
    for (;;) {
        myrtos_msg_t m;
        int32_t from = myrtos_receive_tmo(&m, 5000);

        if (from == MYRTOS_RECV_TIMEOUT) {
            myrtos_write_str(MYRTOS_STDOUT, "armtest: nothing more; done\n");
            break;
        }
        if (from != 0) {                    // a real message, which we do not expect
            myrtos_reply(0);
            continue;
        }

        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  pulse type ");
        myrtos_line_u32(&line, m.type);
        myrtos_line_str(&line, m.type == PULSE_CONSOLE ? " (stdin)" : " (second path)");
        myrtos_line_str(&line, ", descriptor ");
        myrtos_line_u32(&line, m.len);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);

        // First one wins: the rest are of no interest now, and each would
        // otherwise fire one stray pulse into a receive that is no longer
        // expecting it. This is what disarm_all is for.
        int32_t dropped = myrtos_disarm_all();
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  first one wins; dropped ");
        myrtos_line_u32(&line, (uint32_t)dropped);
        myrtos_line_str(&line, " (watches and pulses already sent)\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);

        uint8_t ch = 0;
        if (myrtos_readable((int32_t)m.len) > 0) myrtos_read((int32_t)m.len, &ch, 1);
    }

    myrtos_arm(MYRTOS_STDIN, 0);
    if (second >= 0) { myrtos_arm(second, 0); myrtos_close(second); }
}
