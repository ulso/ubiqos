#include "../../common/myrtos_abi.h"

// armtest -- wait for input and a deadline in the same place.
//
// Before arming there was no way to do this. myrtos_read blocks on one
// descriptor and gives you nothing else; myrtos_receive_tmo blocks on messages
// and a clock and knows nothing about descriptors. A program that wanted "the
// dongle answered, or three seconds passed, whichever comes first" had to poll
// with myrtos_readable and burn its quantum doing it.
//
// Armed, the descriptor sends a pulse when it has something, and then there is
// one waiting place: receive with a deadline. A keystroke, a reply from a
// server and the clock all arrive there.
#define PULSE_INPUT 7

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    myrtos_line_t line;

    if (myrtos_arm(MYRTOS_STDIN, PULSE_INPUT) != 0) {
        myrtos_write_str(MYRTOS_STDOUT, "armtest: no room to watch\n");
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "armtest: waiting 5 s for a key...\n");

    myrtos_msg_t m;
    int32_t from = myrtos_receive_tmo(&m, 5000);

    myrtos_line_reset(&line);
    if (from == MYRTOS_RECV_TIMEOUT) {
        myrtos_line_str(&line, "armtest: timed out, nothing arrived\n");
    } else if (from == 0 && m.type == PULSE_INPUT) {
        uint8_t ch = 0;
        myrtos_read(MYRTOS_STDIN, &ch, 1);
        myrtos_line_str(&line, "armtest: a pulse from the kernel, descriptor ");
        myrtos_line_u32(&line, m.len);       // the value is the descriptor
        myrtos_line_str(&line, ", and the key was '");
        myrtos_line_chars(&line, (const char*)&ch, 1);
        myrtos_line_str(&line, "'\n");
    } else {
        myrtos_line_str(&line, "armtest: something else arrived, from ");
        myrtos_line_u32(&line, (uint32_t)from);
        myrtos_line_str(&line, "\n");
    }
    myrtos_line_flush(MYRTOS_STDOUT, &line);

    myrtos_arm(MYRTOS_STDIN, 0);             // cancel, in case it never fired
}
