#include "usbdev.h"
#include "config.h"
#include "clock.h"
#include "tusb.h"
#include "../common/modules.h"   // myrtos_sleep, through the shared ABI
#include "pico/time.h"                 // time_us_64, for the event log

// How long the bus must stay quiet before the CDC-NCM link is called down.
// Four times the longest idle suspend seen on this bench, and short enough
// that a cable actually pulled out is noticed while the user is still
// looking at the screen.
#define MYRTOS_USB_QUIET_US (15ull * 1000000ull)

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#if !MYRTOS_USB_NATIVE_HOST

void myrtos_usb_init(void) {
    // TinyUSB's RP2040 port registers itself for USBCTRL_IRQ through the SDK's
    // irq_add_shared_handler. That only works now that we stopped taking over
    // mtvec: the SDK's external dispatch is what looks up in that table.
    tud_init(0);

    // And stay OFF the bus until something can answer it. tud_connect is in
    // usb_thread, below.
    //
    // This is the fix for the morning's fix. Telling the host we are here is
    // one thing; being able to reply is another, and between this line and the
    // start of the USB task the kernel still has flash to scan, descriptors to
    // register and a shell to create. A host that enumerates in that window
    // gets no answer: every SETUP packet becomes an event queued by the
    // interrupt handler for a tud_task that is not running yet. The queue holds
    // sixteen. When it is full TU_ASSERT drops the event -- and drops it
    // WITHOUT clearing the hardware status bit that caused it, so the interrupt
    // fires again immediately, for ever. The board then never finishes booting:
    // myrtos_ticks stays at 0 because pre-emption is enabled on the last line
    // of main, and the video pump stops after about eighty passes.
    //
    // The window was always there. What changed on 11 Sep 2026 is that the
    // pulse below made the host enumerate promptly EVERY time, where before it
    // frequently did not -- which was the bug the pulse was written to fix. So
    // the pulse did not create the hazard, it just stopped hiding it.
    //
    // Leaving the pull-up down until the task is running closes both: the host
    // still sees a clean disconnect after a warm reset, and the gap from here
    // to the task is far longer than the 120 ms busy wait this replaces.
    tud_disconnect();

    // Then tell the host, in a way it cannot miss, that whatever used to be on
    // this port is gone.
    //
    // tud_init resets the USB block and raises the pull-up again within
    // microseconds. A host reading the line directly might notice that; one
    // behind a hub will not, because a hub debounces a disconnect over tens of
    // milliseconds before it reports one. So after a warm reset -- which is
    // exactly what the debug probe does after `program ... verify reset` -- the
    // Mac goes on believing the previous device is still attached, keeps its
    // address assigned, and never enumerates us. The board then runs perfectly
    // with nothing on the wire: no console, and no network either, because the
    // USB task is the only thread allowed to touch lwIP.
    //
    // That was diagnosed the long way round, through the probe: ticks and video
    // pumps advancing, the crash record empty, every USB counter still zero,
    // and the controller reporting itself connected. Dropping the pull-up by
    // hand for a moment brought the whole machine back. This is that, done at
    // boot so nobody has to do it by hand again.
    //
    // A hub debounces a disconnect over tens of milliseconds and never reports
    // one shorter, which is why the reset's own microseconds are not enough.
    // The rest of the boot is the wait, and it costs nothing.
    myrtos_print("USB device started; the bus is joined once the task can answer it\n");
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

// --- WHAT THE HOST DOES TO US -----------------------------------------------
//
// TinyUSB calls these from tud_task, which runs in this thread, so printing
// from them is ordinary thread context and safe.
//
// They exist because of a question that could not be answered: a network over
// USB does not come back by itself when the Mac wakes from sleep, here and in
// every other project on this bench that carries CDC-NCM. The easy answer is
// that macOS is at fault -- and the easy answer is suspect, because USB
// ethernet dongles speak the same protocol and survive sleep, so the host can
// evidently do it.
//
// Before the blame can be placed, this end has to be able to say whether it
// even saw the host go away. It could not: there was no handler for suspend,
// resume, mount or unmount anywhere in the kernel, and the netif's link was
// set up once at boot and never taken down again. So myrtos could not notice,
// could not recover, and could not report.
//
// This is the noticing. The recovering comes after, and only once the log says
// what actually happens.
//
// Note for reading the log afterwards: this board is on a powered hub, so it
// keeps running while the Mac sleeps and the events below are a real record of
// the bus. Plugged straight into the Mac it may lose power instead, and then
// the absence of any of these lines means something different -- Ulf's idea,
// and the second half of the experiment.
uint32_t myrtos_usb_suspends, myrtos_usb_resumes;
uint32_t myrtos_usb_mounts, myrtos_usb_unmounts;
uint32_t myrtos_usb_last_event_ms;

// Enough of them to see a pattern, then silence. A host that suspends whenever
// the bus goes idle would otherwise fill the log with the same line and push
// out the boot messages, which are the other thing a morning-after reading
// wants. The counters go on counting either way.
#define USB_EVENT_LOG_LIMIT 12u

// Seconds since boot, and the wall clock too once the network has told us what
// it is. The uptime is never dropped: it is the only one of the two that is
// certainly there, it is what the counters are in, and a log that changes
// format halfway through the night is harder to read than one that does not.
static void say_when(uint32_t count) {
    myrtos_print(" at ");
    myrtos_print_u32((uint32_t)(time_us_64() / 1000000u));
    myrtos_print("s");

    char hhmmss[12];
    myrtos_clock_time_only(hhmmss, sizeof hhmmss);
    if (hhmmss[0]) {
        myrtos_print(" (");
        myrtos_print(hhmmss);
        myrtos_print(")");
    }

    myrtos_print(" (");
    myrtos_print_u32(count);
    myrtos_print(")\n");
}

static void note_event(const char *what, uint32_t count) {
    myrtos_usb_last_event_ms = (uint32_t)(time_us_64() / 1000u);
    if (count > USB_EVENT_LOG_LIMIT) return;

    myrtos_print("usb: host ");
    myrtos_print(what);
    say_when(count);
}

// The link transitions, counted and logged separately from the bus events that
// cause them. They are what the direct-to-the-Mac night is for: a bus that
// suspends for hours should produce exactly one down and one up, and any more
// than that means MYRTOS_USB_QUIET_US is too short for this host rather than
// that the design is wrong.
uint32_t myrtos_net_link_ups, myrtos_net_link_downs;

static void note_link(const char *what, uint32_t count) {
    if (count > USB_EVENT_LOG_LIMIT) return;
    myrtos_print("net: cable link ");
    myrtos_print(what);
    say_when(count);
}

// The longest the bus has ever been quiet, and when that started. This exists
// because counting suspends turned out to answer the wrong question: a host
// that suspends once a minute for three seconds and a host that suspends once
// for three hours both raise the counter, and only one of them is a host that
// went away. The log cannot settle it either -- it stops after twelve events on
// purpose, so a chatty host cannot push the boot messages out of the ring, and
// an idle Mac reaches twelve in twelve minutes.
//
// So measure the thing itself. One comparison per resume, two words of state,
// and a night's worth of once-a-minute noise cannot hide a real absence in it.
uint32_t myrtos_usb_longest_quiet_ms;   // the longest suspend seen
uint32_t myrtos_usb_longest_at_s;       // uptime when that one began
static uint64_t quiet_began_us;

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    quiet_began_us = time_us_64();
    note_event("suspended the bus", ++myrtos_usb_suspends);
}

void tud_resume_cb(void) {
    if (quiet_began_us) {
        const uint64_t quiet = time_us_64() - quiet_began_us;
        const uint32_t ms = (uint32_t)(quiet / 1000u);
        if (ms > myrtos_usb_longest_quiet_ms) {
            myrtos_usb_longest_quiet_ms = ms;
            myrtos_usb_longest_at_s = (uint32_t)(quiet_began_us / 1000000u);

            // Always said, however many events have gone before: a new longest
            // is rare and is the one line a morning-after reading wants.
            char hhmmss[12];
            myrtos_clock_time_only(hhmmss, sizeof hhmmss);
            myrtos_print("usb: longest quiet so far ");
            myrtos_print_u32(ms / 1000u);
            myrtos_print("s, which began at ");
            myrtos_print_u32(myrtos_usb_longest_at_s);
            myrtos_print("s");
            if (hhmmss[0]) { myrtos_print(" ("); myrtos_print(hhmmss); myrtos_print(")"); }
            myrtos_print("\n");
        }
        quiet_began_us = 0;
    }
    note_event("resumed the bus", ++myrtos_usb_resumes);
}

void tud_mount_cb(void) {
    note_event("configured us", ++myrtos_usb_mounts);
}

void tud_umount_cb(void) {
    note_event("dropped us", ++myrtos_usb_unmounts);
}

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
// Leave the bus and join it again, so the host enumerates us afresh.
//
// This is the fix for a network that never comes back after the Mac sleeps, and
// the fault is ours rather than the host's. TinyUSB's ncm_device.c says so in
// its own words: "Notifications are transferred to the host once during
// connection setup." Its state machine runs SPEED -> CONNECTED -> DONE, and in
// DONE it sends nothing. netd_reset, which clears that state, runs on a BUS
// RESET only.
//
// A host sleep suspends and resumes without re-enumerating -- measured over a
// 12.5 hour night on 12 Sep 2026: fourteen suspend/resume pairs, the longest
// quiet 16m24s, and mounts stayed at 1 with unmounts at 0 throughout. So
// CDC_NOTIF_NETWORK_CONNECTION was sent once, at boot, and never again. The
// board's own interface was perfectly up with its address; the Mac's end sat
// `status: inactive` with none. USB ethernet dongles survive sleep because they
// re-assert the link. We did not.
//
// There is no public call for it -- net_device.h offers xmit and recv and
// nothing else -- so the re-introduction has to be the whole device. That is
// heavier than it sounds only if it were frequent: this ran nine times in a
// night, once per real sleep, which is what a person does by hand with the
// cable when the network does not come back.
//
// Done HERE, from the USB task, and never from boot: the same call at boot is
// what wedged the machine on 11 Sep, because between tud_init and this task
// there is nobody to answer an enumerating host. See myrtos_usb_init.
static void present_again(void) {
    myrtos_print("net: introducing this board to the host again\n");
    tud_disconnect();
    myrtos_sleep(150);          // past a hub's debounce; a yield, not a spin
    tud_connect();
}

static void usb_thread(void) {
    // NOW join the bus. myrtos_usb_init left the pull-up down on purpose: from
    // here on there is a thread calling tud_task, so an enumerating host gets
    // answered instead of filling a queue nobody drains. See the note there.
    tud_connect();

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
            extern bool myrtos_lwip_set_link(bool);
            static bool up;
            // Not before the card has been read: the hostname is in
            // /sd/config.txt, mDNS announces it once, and a responder that has
            // already said "myrtos" cannot unsay it. myrtos_config_done goes
            // true whether or not there was a card, so a board with no card
            // waits only as long as the driver takes to find that out.
            //
            // And nothing here waits for a USB host any more. It used to also
            // require tud_ready(), which tied the radio to the wrong cable: on
            // a charger, or on a Mac that never enumerated us, the board came
            // up with a display and a keyboard and no network at all. Only the
            // CDC-NCM interface needs a host, and it is told separately, just
            // below.
            if (!up && myrtos_config_done()) {
                extern void myrtos_lwip_sock_init(void);
                myrtos_lwip_start();
                myrtos_lwip_sock_init();   // this thread is stack 1's server
                up = true;
            }
            if (up) {
                // The link follows the host, but not instantly downwards.
                //
                // This host suspends and resumes the bus about once a minute,
                // three or four seconds at a time, with nothing wrong and
                // nobody touching the cable. Taking the link down for that
                // re-announced the name over mDNS every minute -- and, while
                // the address came from AutoIP, took the address away for as
                // long as it took to probe again.
                // A pause is not an unplug.
                //
                // So the link falls only once the bus has stayed quiet for
                // longer than any of those pauses, and rises the moment the
                // host is back. Ulf saw the pattern in the suspend log and
                // asked the question before this was written the naive way.
                static uint64_t quiet_since;
                static bool owe_the_host_an_introduction;

                if (tud_ready()) {
                    quiet_since = 0;
                    if (myrtos_lwip_set_link(true))
                        note_link("up", ++myrtos_net_link_ups);

                    // And say hello again, properly. See present_again below --
                    // this is the one thing that brings the host's end of the
                    // cable back after it has slept.
                    if (owe_the_host_an_introduction) {
                        owe_the_host_an_introduction = false;
                        present_again();
                    }
                } else if (!quiet_since) {
                    quiet_since = time_us_64();
                } else if (time_us_64() - quiet_since > MYRTOS_USB_QUIET_US) {
                    if (myrtos_lwip_set_link(false)) {
                        note_link("down", ++myrtos_net_link_downs);
                        owe_the_host_an_introduction = true;
                    }
                }

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
                // The clock, once there is a route out. Same context, same
                // rule: this calls into lwIP.
                extern void myrtos_sntp_poll(void);
                myrtos_sntp_poll();

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
    // Six kilobytes, and it was four.
    //
    // This one thread carries TinyUSB, lwIP with two interfaces, DHCP, the
    // mDNS responder and the querier, TCP, and the socket server -- everything
    // that has been added to the network since the 9th went in here, because
    // this is the only context lwIP may be touched from. Four kilobytes was
    // measured against none of it.
    //
    // This is not a diagnosis. The board stopped twice with the display still
    // running and everything in this thread dead, which is what an overflow
    // here would look like -- and so is a fault, and so is a spin. It is the
    // cheapest of the things it might be to rule out.
    if (myrtos_kernel_thread(usb_thread, 6144, MYRTOS_PRIO_USB) < 0) {
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

#else   // MYRTOS_USB_NATIVE_HOST

// --- NO DEVICE SIDE ----------------------------------------------------------
//
// Built with MYRTOS_NATIVE_USB=host the chip's controller is the host, and all
// of the above is gone: no console on the cable, no card to lend, no network.
// The names stay, and answer the way the device side always has with no
// computer at the other end -- a write that is refused, a read with nothing in
// it -- so the descriptor, the shell and usbdisk need no build of their own.
//
// The thread stays too, for the one job it has that was never the device's:
// taking delivery of what the host core cannot do itself.

uint32_t myrtos_usb_suspends, myrtos_usb_resumes;
uint32_t myrtos_usb_mounts, myrtos_usb_unmounts;
uint32_t myrtos_usb_last_event_ms;

void myrtos_usb_init(void) {
    myrtos_print("USB: the port is a host; no console, disk or network on it\n");
}

void myrtos_usb_task(void) {}
bool myrtos_usb_ready(void) { return false; }
uint32_t myrtos_usb_writable(void) { return 0; }
uint32_t myrtos_usb_available(void) { return 0; }

int32_t myrtos_usb_write(const uint8_t *buf, uint32_t len) {
    (void)buf; (void)len;
    return -1;
}

int32_t myrtos_usb_read(uint8_t *buf, uint32_t len) {
    (void)buf; (void)len;
    return 0;
}

// usb_descriptors.c's, which a host build leaves out: there is no network
// device for an address to belong to.
void myrtos_usb_net_id(const uint8_t *unique, uint32_t n) {
    (void)unique; (void)n;
}

// usbmsc.c's. A card cannot be lent to a computer that is not there, and a
// refusal is what fsserver.c already knows how to report.
bool myrtos_msc_hand_over(void) { return false; }
void myrtos_msc_take_back(void) {}
bool myrtos_msc_host_has_card(void) { return false; }

static void usb_thread(void) {
    for (;;) {
        { extern void myrtos_usbhost_drain(void); myrtos_usbhost_drain(); }
        myrtos_sleep(1);
    }
}

void myrtos_usb_start_task(void) {
    extern int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes,
                                        uint32_t priority);
    // Four kilobytes: the drain prints, moves the console and interrupts
    // processes, and nothing of the network is here to need the other two.
    if (myrtos_kernel_thread(usb_thread, 4096, MYRTOS_PRIO_USB) < 0) {
        myrtos_print("USB: could not start its service process\n");
    }
}

#endif  // MYRTOS_USB_NATIVE_HOST
