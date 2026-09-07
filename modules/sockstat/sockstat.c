#include "../../common/myrtos_abi.h"

// sockstat -- what the coprocessor thinks each socket is doing, beside what
// this machine thinks.
//
// It exists because an evening went into arguing about a port that would not
// serve, with three explanations that all fitted and no way to tell them apart.
// The chip has an opinion -- GET_STATE_TCP -- and it was simply never asked.
//
// A row where the two disagree is the interesting one: a socket this side has a
// port for and the chip calls closed is a server that has gone without saying
// so, and a socket the chip calls listening with no owner is one waiting to be
// adopted by the next process that asks for its port.

// nina-fw's socket count. Asking about one it does not have is not a harmless
// question: the first version of this walked sixteen and took the network down
// with it.
#define SOCKETS 10

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: sockstat\n\nWhat the WiFi coprocessor says about each socket.\n"))
        return;

    myrtos_line_t l;
    myrtos_write_str(MYRTOS_STDOUT, "sock  state        port   owner\n");

    bool any = false;
    for (int32_t i = 0; i < SOCKETS; i++) {
        // This side first, and it is not an optimisation. The owner and the
        // port are read out of the driver's own table and cost nothing; the
        // state is an SPI transaction with the coprocessor. Asking about a
        // socket nobody has any record of is a question with no reason to be
        // asked, and every such question is a chance to upset the chip.
        int32_t port  = myrtos_sock_port(i);
        int32_t owner = myrtos_sock_owner(i);
        if (!port && owner == -1) continue;

        int32_t state = myrtos_sock_state(i);
        any = true;

        myrtos_line_reset(&l);
        myrtos_line_u32(&l, (uint32_t)i);
        myrtos_line_str(&l, "     ");
        myrtos_line_str(&l, state < 0 ? "no answer" : myrtos_tcp_state_name((uint32_t)state));
        myrtos_line_str(&l, "   ");
        if (port) myrtos_line_u32(&l, (uint32_t)port); else myrtos_line_str(&l, "-");
        myrtos_line_str(&l, "   ");
        if (owner == -1)      myrtos_line_str(&l, "nobody");
        else if (owner == -2) myrtos_line_str(&l, "REAPED");
        else                  myrtos_line_u32(&l, (uint32_t)owner);
        myrtos_line_str(&l, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }
    if (!any) myrtos_write_str(MYRTOS_STDOUT, "(nothing open)\n");
}
