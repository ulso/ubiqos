#include "usbdev.h"
#include "config.h"
#include "tusb.h"
#include "../common/modules.h"   // myrtos_sleep, through the shared ABI

void myrtos_print(const char *s);

void myrtos_usb_init(void) {
    // TinyUSB's RP2040 port registers itself for USBCTRL_IRQ through the SDK's
    // irq_add_shared_handler. That only works now that we stopped taking over
    // mtvec: the SDK's external dispatch is what looks up in that table.
    tud_init(0);
    myrtos_print("USB device started, CDC console on the USB port\n");
}

// TinyUSB does its work here, not in the interrupt. The kernel's idle loop
// calls it, which suffices: everything time critical happens in the handler.
//
// Ctrl-C is looked for here rather than in the read, because while a command is
// running nobody is reading -- which is precisely when it is typed. Peeking
// rather than draining: the head of the FIFO is all TinyUSB will show without
// consuming, and everything behind it is type-ahead the shell is owed.
//
// So it is caught when it is the next byte, which it is unless something was
// typed first and left unread. Draining into a buffer of our own would close
// that gap and cost a quarter kilobyte, which this machine has not got.
bool myrtos_io_interrupt(const char *device_name);

void myrtos_usb_task(void) {
    tud_task();

    uint8_t c;
    if (tud_mounted() && tud_cdc_available() && tud_cdc_peek(&c) && c == 3) {
        if (myrtos_io_interrupt("usb")) {
            tud_cdc_read(&c, 1);        // consumed: it was never data
        }
    }
}

bool myrtos_usb_ready(void) {
    return tud_cdc_connected();
}

int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len) {
    // No gate on tud_cdc_connected(): it mirrors DTR, and a host that opened
    // the port without asserting DTR then had its data silently dropped.
    // TinyUSB buffers, and discards on its own when nobody is listening.
    if (!tud_mounted()) return -1;

    // Writes as much as fits and says how much that was. It used to call
    // tud_task here when the buffer filled, which cannot work: this runs in the
    // trap handler with interrupts off, so the transfer that would drain the
    // buffer can never complete, and the USB process may be inside tud_task at
    // the same moment. The text was simply cut off at 256 bytes.
    static char last_out;       // the byte before this one, for the CR above
    uint32_t written = 0;
    while (written < len) {
        // The same translation the UART driver does: a LONE line feed is
        // preceded by a carriage return. Without it the output staircases to
        // the right, and every utility would have to write \r\n itself. The
        // pair goes in together or not at all, so a retry cannot repeat the CR.
        //
        // Lone is the point. It used to add one to every line feed, so a stream
        // that already had its own came out as \r\r\n -- which a terminal
        // forgives and a program passing bytes through should not be doing at
        // all. Remembered across calls, because a write may end on the CR.
        bool lone = (buf[written] == '\n') && (last_out != '\r');
        uint32_t need = lone ? 2u : 1u;
        if (tud_cdc_write_available() < need) break;
        if (lone) { char cr = '\r'; tud_cdc_write(&cr, 1); }
        tud_cdc_write(buf + written, 1);
        last_out = (char)buf[written];
        written++;
    }
    if (written) tud_cdc_write_flush();
    return (int32_t)written;
}

// USB is serviced by a process of its own, high enough that an application
// cannot silence the console by being busy. One millisecond is far more often
// than needed: CDC data has no deadline at all -- a late poll costs throughput,
// since the host simply retries -- and the tightest real limit is the 50 ms USB
// gives a device to answer a standard request with no data stage. Fifty times
// the margin, for a sleep that costs nothing.
static void usb_thread(void) {
    for (;;) {
        myrtos_usb_task();          // the console, on the hardware controller

        // lwIP lives HERE and nowhere else. NO_SYS is 1, so it has no locking
        // of its own: the frames arrive in tud_network_recv_cb, which is called
        // from tud_task just above, and the timers are driven from the same
        // loop. Any other context calling into lwIP would be a race with no
        // symptom until it had one.
#if MYRTOS_LWIP
        {
            extern void myrtos_lwip_start(void);
            extern void myrtos_lwip_poll(void);
            extern bool myrtos_eh_netif_start(void);
            extern bool myrtos_eh_netif_up(void);
            extern void myrtos_eh_netif_poll(void);
            static bool up;
            // And not before the card has been read: the hostname is in
            // /sd/config.txt, mDNS announces it once, and a responder that has
            // already said "myrtos" cannot unsay it. myrtos_config_done goes
            // true whether or not there was a card, so a board with no card
            // waits only as long as the driver takes to find that out.
            if (!up && tud_ready() && myrtos_config_done()) {
                extern void myrtos_lwip_sock_init(void);
                myrtos_lwip_start();
                myrtos_lwip_sock_init();   // this thread is stack 1's server
                up = true;
            }
            if (up) {
                myrtos_lwip_poll();

                // The WiFi interface, in the same turn and the same context.
                // lwIP may not be touched from anywhere else, and the ESP
                // transport is a thread of its own -- so the frames it queues
                // are taken here or not at all. Started rather than polled
                // into existence: it needs the radio's own hardware address,
                // which only exists once the control plane has asked for it.
                if (!myrtos_eh_netif_up()) myrtos_eh_netif_start();
                else                       myrtos_eh_netif_poll();

                // And the socket server, in the same turn and the same
                // context: lwIP may not be touched from anywhere else, so the
                // process that answers socket calls for stack 1 has to be this
                // one. Zero milliseconds, so a turn with nothing waiting costs
                // a single look.
                extern void myrtos_lwip_serve(void);
                myrtos_lwip_serve();
            }
        }
#endif

        // The keyboard is not here any more. tuh_task, the repeat clock and the
        // rearm sweep all run on core 1; what is left on this side is taking
        // delivery of what they could not do themselves -- an interrupt for a
        // process, a scrollback move, a line of log.
        { extern void myrtos_usbhost_drain(void); myrtos_usbhost_drain(); }
        myrtos_sleep(1);


    }
}

void myrtos_usb_start_task(void) {
    extern int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes,
                                        uint32_t priority);
    if (myrtos_kernel_thread(usb_thread, 4096, MYRTOS_PRIO_USB) < 0) {
        myrtos_print("USB: could not start its service process\n");
    }
}

uint32_t myrtos_usb_writable(void) {
    return tud_mounted() ? tud_cdc_write_available() : 0;
}

uint32_t myrtos_usb_available(void) {
    return tud_cdc_connected() || tud_mounted() ? tud_cdc_available() : 0;
}

int32_t myrtos_usb_read(uint8_t *buf, uint32_t len) {
    if (!tud_mounted() || !tud_cdc_available()) return 0;
    return (int32_t)tud_cdc_read(buf, len);
}
