#include "../common/modules.h"   // the keymap type, through the shared ABI
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "board.h"
#if !UBIQOS_USB_NATIVE_HOST
#include "pio_usb.h"
#include "pio_usb_ll.h"
#endif
#include "hardware/structs/sysinfo.h"
#include "tusb.h"
#include "chargen.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "keystore.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);

// A USB host on two PIO state machines, so a keyboard can be plugged in while
// the hardware controller stays busy being our console.
//
// The pins are the board's, from its header. D- is always D+ plus one, which
// PIO-USB requires; the rest a board may simply not have.
#define USB_HOST_DP_PIN  UBIQOS_USB_HOST_DP

#if UBIQOS_HAS_USB_HOST_POWER
#define USB_HOST_POWER   UBIQOS_USB_HOST_POWER
#endif

// Some boards hold their peripherals in reset until a pin is driven high. The
// USB hub behind the Fruit Jam's two host sockets is one of them, so nothing
// enumerates while it is low -- which looks exactly like a host that is not
// working. A board with a socket soldered straight to the pads has neither the
// hub nor the pin.
#if UBIQOS_HAS_PERIPH_RESET
#define PERIPH_RESET     UBIQOS_PERIPH_RESET
#define ESP_BOOT         UBIQOS_ESP_BOOT_STRAP   // the ESP32-C6's GPIO9, and the BOOT button
#endif

static uint8_t keys[32];
static volatile uint32_t head, tail;

// The key ring has TWO producers, which is easy to miss: the keyboard fills it,
// and console.c answers a cursor-position report through it as well, because a
// terminal's reply to a program goes to that program's input. One core made
// that harmless. Two do not, so it takes a hardware spinlock -- disabling
// interrupts would only protect it from the core already holding it.
static spin_lock_t *keylock;

// The key queue is a data structure and not a device, so its lock is claimed
// whether or not a host is ever started.
//
// It used to be claimed inside ubiqos_usbhost_init, which a board with no PIO
// USB host does not call -- and then keylock stayed NULL and the first read
// spun on address zero for ever. That deadlocked the Waveshare board the moment
// a shell ran on its panel: `con` reads the keyboard, and a console with no
// keyboard still reads it. The probe found it in spin_lock_unsafe_blocking with
// the tick frozen thirteen milliseconds after pre-emption started.
void ubiqos_usbhost_queue_init(void)
{
    if (!keylock) keylock = spin_lock_instance((uint)spin_lock_claim_unused(true));
}

// --- WHAT CORE 1 MAY NOT DO ITSELF -----------------------------------------
//
// The USB host is moving to the second core, and the rule is the one already
// written for a handler above the kernel's threshold: touch your own memory,
// call nothing. Three things in here broke that -- delivering an interrupt to a
// process, moving the console's scrollback, and printing -- and all three are
// reached from the key path, which already had a ring. So they become events
// instead, and core 0 does the work when it drains them.
//
// Nothing carries a pointer. The log messages are ids and numbers, so no string
// crosses between the cores at all.
enum { EV_LOG = 1, EV_INTR, EV_VIEW };
enum { LOG_MOUNT = 1, LOG_UMOUNT, LOG_HID_KBD, LOG_HID_OTHER, LOG_ARMED,
       LOG_HID_GONE, LOG_CDC_UP, LOG_CDC_GONE, LOG_STARTED, LOG_INIT_FAIL,
       LOG_NO_SLOT };
enum { VIEW_MOVE = 1, VIEW_HOME, VIEW_END };

typedef struct { uint8_t kind, id; uint32_t a, b; } usb_event_t;
static usb_event_t evq[32];
static volatile uint32_t ev_head, ev_tail;

static void ev_push(uint8_t kind, uint8_t id, uint32_t a, uint32_t b) {
    uint32_t next = (ev_head + 1u) % (sizeof(evq) / sizeof(evq[0]));
    if (next == ev_tail) return;            // full: an event is dropped, not queued late
    evq[ev_head].kind = kind;
    evq[ev_head].id = id;
    evq[ev_head].a = a;
    evq[ev_head].b = b;
    __dmb();                                 // the entry before the index that publishes it
    ev_head = next;
}

// TinyUSB asks the port for the time. It is declared in tusb.h and defined
// nowhere in the SDK, so it is ours to supply. The hardware clock rather than
// our tick, because tuh_init runs before the scheduler's timer starts.
uint32_t tusb_time_millis_api(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

// The board's own power-up, which is NOT the USB host and must not move with
// it: GP22 releases the hub, the audio DAC and the ESP32-C6 together, and the
// drivers that come later in boot depend on having happened after it. Only the
// PIO half belongs on the other core.
void ubiqos_usbhost_init(void) {
    ubiqos_usbhost_queue_init();

    // GP22 releases the USB hub, the audio DAC and the ESP32-C6 together, so the
    // ESP's reset happens here whether or not anybody wants WiFi.
    //
    // And it has to happen with GP0 high. The ESP samples its GPIO9 as it leaves
    // reset -- low means the serial bootloader, high means run the firmware --
    // and that pin is wired to GP0 on this board. An RP2350 pin comes out of
    // reset as an input with its PULL-DOWN on, so GP0 was holding the ESP in
    // bootloader mode every time. The chip had power and drove its busy line,
    // which is what made it look present but permanently not ready.
#if UBIQOS_HAS_PERIPH_RESET
    gpio_init(ESP_BOOT);
    gpio_set_dir(ESP_BOOT, GPIO_IN);
    gpio_set_pulls(ESP_BOOT, true, false);   // pull up, and leave the button alone
    sleep_ms(1);

    gpio_init(PERIPH_RESET);
    gpio_set_dir(PERIPH_RESET, GPIO_OUT);
    gpio_put(PERIPH_RESET, 0);            // a real pulse, not just a release
    sleep_ms(10);
    gpio_put(PERIPH_RESET, 1);            // let the on-board peripherals go
#endif

#if UBIQOS_HAS_USB_HOST_POWER
    gpio_init(USB_HOST_POWER);
    gpio_set_dir(USB_HOST_POWER, GPIO_OUT);
    gpio_put(USB_HOST_POWER, 1);          // the port is dead without this
#else
    // The 5V is wired permanently on this board, so the port is live from the
    // moment it has power and there is no way to cycle it from software. Worth
    // knowing: on the Fruit Jam a power cycle was more than once the only thing
    // that recovered a device that had wedged its end of the bus.
#endif

    sleep_ms(100);                        // a hub, if there is one, needs a moment
}

// --- THE SECOND CORE -------------------------------------------------------
//
// tuh_configure and tuh_init run HERE and not on core 0, because the interrupts
// they set up belong to whichever core enables them. PIO-USB drives its start
// of frame from a repeating timer every millisecond, and that is the one thing
// on this board that genuinely cannot be late -- putting it on a core that also
// builds 31000 scanlines a second is what took the keyboard down.
//
// Nothing below reaches into the kernel. Keys go through a spinlocked ring and
// everything else through the event queue, which core 0 drains. That is the
// rule already written for a handler above the kernel's threshold, applied to a
// core instead of a handler.
void ubiqos_usbhost_repeat(void);
void ubiqos_usbhost_rearm(void);

// Core 1's pulse, for the question a USB fault asks first and that nothing else
// could answer: is the host loop running at all? The debugger cannot halt core 1
// on this part -- not in a fault and not in a healthy board either, which was
// learned the hard way by concluding the opposite from its silence.
static volatile uint32_t core1_beats;
static volatile uint32_t core1_last_ms;

static void core1_main(void)
{
#if !UBIQOS_USB_NATIVE_HOST
    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = USB_HOST_DP_PIN;

    // kernel/videorgb.c keeps this DMA channel free by number, because the
    // library claims it by number and would otherwise claim one the video had
    // already taken. If the default ever moves, that reservation guards the
    // wrong channel, so say so here rather than let the panel freeze again.
    static_assert(PIO_USB_DMA_TX_DEFAULT == 0,
                  "videorgb.c reserves DMA channel 0 for this");

    tuh_configure(CFG_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
#endif
    // The chip's own controller needs no configuring: its interrupt is taken
    // on this core by tuh_init, exactly as PIO-USB's timer is.

    if (!tuh_init(CFG_TUH_RHPORT)) {
        ev_push(EV_LOG, LOG_INIT_FAIL, 0, 0);
        for (;;) tight_loop_contents();
    }
    ev_push(EV_LOG, LOG_STARTED, 0, 0);

    for (;;) {
        // Counted at the TOP of the pass, so that a tuh_task which never
        // returns shows up as an age that grows rather than a count that is
        // merely low. Written by core 1 and read by core 0: a word, aligned,
        // which this machine stores in one go -- no lock, and none wanted on
        // the path that has to work when everything else has stopped.
        core1_beats++;
        core1_last_ms = tusb_time_millis_api();

        ubiqos_keys_park_here();     // core 0 wants the flash to itself
        tuh_task();
        ubiqos_usbhost_repeat();
        ubiqos_usbhost_rearm();
        busy_wait_us(200);
    }
}

void ubiqos_usbhost_start_core1(void)
{
    ubiqos_keys_core1_running = true;
    multicore_launch_core1(core1_main);
}

void ubiqos_usbhost_task(void) { tuh_task(); }

bool ubiqos_io_interrupt(const char *device_name);
static void push_locked(const char *sq, uint32_t n);

// --- CORE 0 DRAINS WHAT CORE 1 COULD NOT DO --------------------------------
// Everything here touches the kernel, and that is the point: it runs on the
// core that owns it. The events carry ids and numbers, never a pointer, so
// nothing that crosses can dangle.
void ubiqos_usbhost_drain(void)
{
    while (ev_tail != ev_head) {
        usb_event_t e = evq[ev_tail];
        __dmb();
        ev_tail = (ev_tail + 1u) % (sizeof(evq) / sizeof(evq[0]));

        switch (e.kind) {
        case EV_INTR:
            // Nobody wanted it, so it is a character after all -- which is what
            // the old code decided at the keyboard, in a place that no longer
            // knows enough to decide it.
            if (!ubiqos_io_interrupt("con") && !ubiqos_io_interrupt("kbd")) {
                char c = 3;
                push_locked(&c, 1);
            }
            break;

        case EV_VIEW:
#if UBIQOS_VIDEO_CHARGEN
            if (e.id == VIEW_MOVE) ubiqos_chargen_view_move((int32_t)e.a);
            else if (e.id == VIEW_HOME) ubiqos_chargen_view_home();
            else ubiqos_chargen_view_end();
#endif
            break;

        case EV_LOG:
            switch (e.id) {
            case LOG_MOUNT:
            case LOG_UMOUNT:
                ubiqos_print("USB host: device ");
                ubiqos_print_u32(e.a);
                ubiqos_print(e.id == LOG_MOUNT ? " attached\n" : " removed\n");
                break;
            case LOG_HID_KBD:   ubiqos_print("USB host: keyboard ready\n"); break;
            case LOG_HID_OTHER: ubiqos_print("USB host: HID device, not a keyboard\n"); break;
            case LOG_NO_SLOT:   ubiqos_print("USB host: no free HID slot; this device will not be polled\n"); break;
            case LOG_ARMED:     ubiqos_print("USB host:   armed\n"); break;
            case LOG_HID_GONE:  ubiqos_print("USB host: HID gone\n"); break;
            case LOG_CDC_GONE:  ubiqos_print("USB host: CDC-ACM device gone\n"); break;
            case LOG_STARTED:
                // The numbers, not a remembered pair of them. This line said
                // "D+ GP1, power GP11" on a board whose D+ is GP0 and which has
                // no power pin at all -- printed because starting the host on
                // core 1 had been left unguarded, and believed because the line
                // could not disagree with the code.
#if UBIQOS_USB_NATIVE_HOST
                // And the same mistake the other way: a host on the chip's own
                // controller said it was PIO on GP0 and GP1, which it has left.
                ubiqos_print("USB host on core 1, the chip's own controller");
#else
                ubiqos_print("USB host on core 1, PIO, D+ GP");
                ubiqos_print_u32(USB_HOST_DP_PIN);
                ubiqos_print(", D- GP");
                ubiqos_print_u32(USB_HOST_DP_PIN + 1);
#if UBIQOS_HAS_USB_HOST_POWER
                ubiqos_print(", power GP");
                ubiqos_print_u32(USB_HOST_POWER);
#else
                ubiqos_print(", 5V always on");
#endif
#endif
                ubiqos_print("\n");
                break;
            case LOG_INIT_FAIL: ubiqos_print("USB host: tuh_init failed\n"); break;
            case LOG_CDC_UP:
                ubiqos_print("USB host: CDC-ACM ready as 'acm', ");
                ubiqos_print_hex(e.a >> 16);
                ubiqos_print(":");
                ubiqos_print_hex(e.a & 0xffffu);
                ubiqos_print(e.b ? ", DTR high\n" : ", DTR low\n");
                break;
            }
            break;
        }
    }
}

// What the driver hands out. A ring, because keys arrive in an interrupt-ish
// context and are read from a system call.
int32_t ubiqos_usbhost_read(uint8_t *buf, uint32_t len) {
    // The reader takes the lock as well, now that the writer is on another
    // core. Reading an index the other core is updating is the same race as
    // writing one.
    uint32_t save = spin_lock_blocking(keylock);
    uint32_t n = 0;
    while (n < len && head != tail) {
        buf[n++] = keys[tail];
        tail = (tail + 1) % sizeof(keys);
    }
    spin_unlock(keylock, save);
    return (int32_t)n;
}

uint32_t ubiqos_usbhost_available(void) {
    return (head - tail) % sizeof(keys);
}

static void push(uint8_t c) {
    uint32_t next = (head + 1) % sizeof(keys);
    if (next != tail) { keys[head] = c; head = next; }
}

// Both halves of a sequence or neither, and now across two cores as well.
static void push_locked(const char *sq, uint32_t n) {
    uint32_t save = spin_lock_blocking(keylock);
    uint32_t free_slots = (tail - head - 1 + sizeof(keys)) % sizeof(keys);
    if (free_slots >= n)
        for (uint32_t i = 0; i < n; i++) push((uint8_t)sq[i]);
    spin_unlock(keylock, save);
}

static void forget_held_keys(void);

// The key the repeat clock is currently sending, 0 when none. Declared up here
// rather than only where it lives, because the callbacks below clear it long
// before the auto-repeat section defines what it means -- which is further
// down, along with what bounds it.
static uint8_t repeat_key;

// --- what TinyUSB calls back ----------------------------------------------

// Defined below with the poll table it clears; declared here because the
// callbacks come first in this file.
static void hid_forget_device(uint8_t addr);

void tuh_mount_cb(uint8_t addr) {
    ev_push(EV_LOG, LOG_MOUNT, addr, 0);
}

void tuh_umount_cb(uint8_t addr) {
    ev_push(EV_LOG, LOG_UMOUNT, addr, 0);

    // Release every HID slot this address held.
    //
    // Only tuh_hid_umount_cb did that, per INTERFACE, and a device can leave
    // without one arriving for each of its interfaces -- or at all. Then the
    // slot stays `wanted` for ever and the re-arm sweep goes on asking a device
    // that is not there.
    //
    // There are eight slots. On 11 Sep 2026 pulling the keyboard out set off a
    // burst of attach/"HID device, not a keyboard"/armed with no umount between
    // them, each one taking a slot and none giving one back; when the keyboard
    // was plugged in again there was no slot left for it, hid_want gave up
    // silently, and the keyboard was simply dead. Nothing said why.
    //
    // The burst itself is somebody else's fault -- see the USB host notes -- but
    // a host stack that leaks its own table over it is ours, and this is the
    // half we can fix: a loop like that should cost log lines, not the keyboard.
    hid_forget_device(addr);
}

// Asking for the next report is the only thing that keeps a keyboard alive, and
// the ask can be refused: tuh_hid_receive_report returns false when the endpoint
// is already busy or the device has gone. Both call sites used to throw that
// answer away -- and a single refused re-arm ends all keyboard input for good,
// without a word, because nothing ever asks again.
//
// That is indistinguishable from the outside from a keyboard that was never
// there: the HID interface stays claimed, the device stays enumerated, and the
// endpoint buffer stays empty for ever. It cost an evening on 31 Aug 2026, when
// a dead keyboard turned out to have a complete and healthy chain behind it.
//
// So what wants a report is remembered, and the ask is repeated until it takes.
#define HID_SLOTS 8
static struct {
    uint8_t addr, instance;
    bool wanted, armed;
    uint8_t idle;                // consecutive sweeps with nothing in flight
    uint8_t dead;                // consecutive sweeps with nothing on the WIRE
    uint8_t tries;               // recoveries since the last report arrived
} hid_poll[HID_SLOTS];

uint32_t ubiqos_hid_rearms;      // how many times the ask had to be repeated
uint32_t ubiqos_hid_lost_repeats; // repeats abandoned because the keyboard went
uint32_t ubiqos_hid_recoveries;  // how many times a submitted transfer was lost
uint32_t ubiqos_hid_no_slot;     // devices turned away because the table was full

static void hid_want(uint8_t addr, uint8_t instance) {
    for (int i = 0; i < HID_SLOTS; i++) {
        if (hid_poll[i].wanted && hid_poll[i].addr == addr && hid_poll[i].instance == instance) {
            hid_poll[i].armed = tuh_hid_receive_report(addr, instance);
            return;
        }
    }
    for (int i = 0; i < HID_SLOTS; i++) {
        if (!hid_poll[i].wanted) {
            hid_poll[i].addr = addr;
            hid_poll[i].instance = instance;
            hid_poll[i].wanted = true;
            hid_poll[i].idle = 0;
            hid_poll[i].dead = 0;
            hid_poll[i].tries = 0;
            hid_poll[i].armed = tuh_hid_receive_report(addr, instance);
            return;
        }
    }

    // And say so. Falling off the end used to be silent, which turned a full
    // table into a keyboard that did nothing for no stated reason -- the worst
    // kind of fault this system has, and one it has had before. Asking for a
    // report is the only thing that keeps a keyboard alive; a device that never
    // gets a slot is never asked.
    ev_push(EV_LOG, LOG_NO_SLOT, addr, instance);
    ubiqos_hid_no_slot++;
}

// Everything this side is holding on behalf of one device address.
static void hid_forget_device(uint8_t addr) {
    for (int i = 0; i < HID_SLOTS; i++) {
        if (hid_poll[i].wanted && hid_poll[i].addr == addr) {
            hid_poll[i].wanted = false;
            hid_poll[i].armed = false;
        }
    }
    if (repeat_key) ubiqos_hid_lost_repeats++;
    forget_held_keys();
}

// Called from the USB process, once round every pass. A refused ask costs one
// more attempt a millisecond later rather than the keyboard.
//
// Watching 'armed' alone was not enough, and the gap took a second evening to
// find. The flag records that the ASK was accepted -- that a transfer was handed
// to the host stack -- and says nothing about whether it ever came back. A
// transfer that is submitted and then lost leaves 'armed' true for ever and this
// sweep skipping the slot for ever. Every flag reads healthy while the keyboard
// is silent: on 3 Sep 2026 both slots stood wanted and armed, the bus was still
// sending a start of frame every millisecond, and not one report had arrived
// since the wasm interpreter ran.
//
// So the question goes to the stack rather than to our own bookkeeping.
// tuh_hid_receive_ready is false while a transfer is genuinely outstanding -- an
// idle keyboard answers each poll with a NAK and the transfer stays pending --
// and true only when there is nothing in flight at all. True while this side
// believes a report is on its way means the transfer is gone, and the ask has to
// be made again.
//
// One ready sweep is not enough to act on: a report just handed over has a
// window before the endpoint reads as busy, and re-arming inside it would submit
// twice. Three consecutive milliseconds with nothing in flight is not a window.
#define HID_IDLE_SWEEPS 3

int32_t ubiqos_usbhost_cdc_index(void);

// The interrupt IN endpoint one HID instance's reports arrive on, or nothing.
//
// The control endpoint is skipped: it is ep 0 in both directions and is never
// what a report comes back on. What is left are the interface endpoints, and
// there is more than one of them -- this very keyboard reports two, 0x81 for
// the keys and 0x82 for the second HID interface beside them. So the instance
// has to be counted off rather than assumed away; taking the first match would
// have watched the keyboard's endpoint on behalf of the other interface and
// drawn the wrong conclusion about both.
//
// The pool is filled in the order the endpoints are opened, which is the order
// the interfaces are mounted, which is the order the instances are numbered.
//
// PIO-USB's own pool, so a host on the chip's controller has no such look and
// goes without this recovery; the ready sweep below still applies to it.
#if !UBIQOS_USB_NATIVE_HOST
static const endpoint_t *hid_in_endpoint(uint8_t addr, uint8_t instance) {
    for (int i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        const endpoint_t *e = PIO_USB_ENDPOINT(i);
        // A closed slot keeps its old dev_addr and ep_num; only size says it is
        // gone. Matching one would have this sweep looking at an endpoint that
        // no longer exists, finding nothing on the wire because there is no
        // wire, and aborting a healthy transfer on the live one every sixteen
        // passes. _find_ep inside the library tests the same field first.
        if (!e->size) continue;
        if (e->dev_addr != addr) continue;
        if (!(e->ep_num & 0x80)) continue;
        if ((e->ep_num & 0x7f) == 0) continue;
        if (instance--) continue;
        return e;
    }
    return 0;
}
#endif

// Long enough that the ordinary gap between one transfer completing and the
// next being queued cannot be mistaken for the fault. That gap is microseconds
// -- the host stack re-queues from the completion callback -- and this is
// sixteen milliseconds, which is also far too short for anyone to notice.
#define HID_DEAD_SWEEPS 16

// A keyboard that has genuinely gone is asked this many times and then left
// alone, so a removed device cannot spin here for ever. The budget is refilled
// by any report that arrives, so a keyboard that recovers is never rationed.
#define HID_RECOVER_LIMIT 8

// The CDC side has the same disease and, unlike the hub, a cure that is public.
//
// cdch_xfer_cb opens with TU_ASSERT(event == XFER_RESULT_SUCCESS) -- upstream's
// own comment above it reads "TODO handle stall response, retry failed transfer"
// -- so one failed transfer returns before the line that queues the next one,
// and the dongle goes deaf. Seen on 4 Sep 2026: the assertion at cdc_host.c:679,
// endpoint 0x81 of the device reading NOTHING QUEUED with three failures, while
// the hub and the keyboard beside it were untouched.
//
// It does not heal by itself, and the reason is a loop that never closes.
// tu_edpt_stream_read queues the next transfer after every read, so an active
// reader keeps the stream alive -- but the kernel only calls acm_read when
// acm_readable says there is something, and with the stream dead there never is.
//
// UNPROVEN AGAINST THE REAL FAULT, and that is worth saying. An endpoint with
// nothing queued is usually not broken at all: when a reader goes away the FIFO
// fills, and tu_edpt_stream_read_xfer stops asking for more until there is room
// again. That is flow control, it heals the moment somebody reads, and it is why
// this refuses to act while the FIFO holds anything. The genuine fault -- the
// one with three failures behind it -- could not be provoked to order, so what
// follows was reasoned out rather than watched. ubiqos_cdc_rearms is there to
// say whether it ever fires.
//
// tuh_cdc_read_clear queues it again. The claim inside fails harmlessly when the
// endpoint is genuinely busy, which is what makes this safe to call speculatively
// -- and it is what the hub's own re-arm lacked, which is why that one asserted
// and was taken out again. It also empties the receive FIFO, so it is only
// called when the FIFO is empty and there is nothing to lose.
// The stall is detected in the PIO layer and not from the return of
// tuh_cdc_read_clear, which says whether the FIFO was emptied and not whether a
// transfer was queued -- counting that would have counted every sweep for ever.
// An IN endpoint of the CDC device with nothing queued is the state, and it has
// to be seen twice sixty-four milliseconds apart, so that the window where PIO
// has finished and the host stack has not yet noticed cannot be mistaken for it.
uint32_t ubiqos_cdc_rearms;

// How many times a silent dongle is asked again before it is left alone.
//
// The re-arm fires about nine times a second, so this is a few seconds of
// trying -- long enough for a device that is merely wedged, and bounded, which
// the first version was not. Measured on a dongle that had stopped answering:
// 3047 re-arms, then 3220 twenty seconds later, and it would have gone on until
// the power was cut. A recovery with no way to give up is not a recovery.
#define CDC_REARM_LIMIT 32

uint32_t ubiqos_cdc_gaveup;

static void cdc_rearm(void) {
    static uint8_t idle_sweeps;
    static uint16_t attempts;

    // Every one of these means the endpoint is healthy or the device is gone,
    // and either way the count starts again -- so a dongle that is unplugged
    // and put back gets the same patience as the first time.
    int32_t idx = ubiqos_usbhost_cdc_index();
    if (idx < 0 || !tuh_cdc_mounted((uint8_t)idx)) {
        idle_sweeps = 0; attempts = 0; ubiqos_cdc_gaveup = 0; return;
    }
    if (tuh_cdc_read_available((uint8_t)idx)) {
        idle_sweeps = 0; attempts = 0; ubiqos_cdc_gaveup = 0; return;
    }

    tuh_itf_info_t info;
    if (!tuh_cdc_itf_get_info((uint8_t)idx, &info)) { idle_sweeps = 0; return; }

    // Read from PIO-USB's pool; on the chip's controller there is no pool to
    // read, so nothing is ever seen as stalled and this does nothing.
    bool stalled = false;
#if !UBIQOS_USB_NATIVE_HOST
    for (int i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        const endpoint_t *e = PIO_USB_ENDPOINT(i);
        if (e->dev_addr != info.daddr) continue;
        if (!(e->ep_num & 0x80u)) continue;
        // Bulk, and only bulk. A CDC device has three IN endpoints -- control,
        // the interrupt one it reports line state on, and the bulk one the data
        // arrives over -- and the first two are legitimately idle almost always.
        // Matching any of them fired this sixty times in the first four seconds.
        if (e->attr != 2) continue;
        if (!e->has_transfer) stalled = true;
    }
#endif
    if (!stalled) { idle_sweeps = 0; attempts = 0; ubiqos_cdc_gaveup = 0; return; }
    if (++idle_sweeps < 2)    return;

    idle_sweeps = 0;

    // Out of patience. Nothing is torn down -- taking a device away from
    // TinyUSB while it believes it owns one is what has produced an ebreak
    // twice today -- it is simply left alone, and usbstat says so.
    if (attempts >= CDC_REARM_LIMIT) { ubiqos_cdc_gaveup = 1; return; }
    attempts++;

    tuh_cdc_read_clear((uint8_t)idx);
    ubiqos_cdc_rearms++;
}

void ubiqos_usbhost_rearm(void) {
    // Not every pass. A healthy endpoint is busy and the claim simply fails, so
    // this costs little either way, but sixty times a second is enough.
    static uint8_t cdc_countdown;
    if (!cdc_countdown--) { cdc_countdown = 63; cdc_rearm(); }

    for (int i = 0; i < HID_SLOTS; i++) {
        if (!hid_poll[i].wanted) continue;

        if (!hid_poll[i].armed) {
            hid_poll[i].armed = tuh_hid_receive_report(hid_poll[i].addr, hid_poll[i].instance);
            hid_poll[i].idle = 0;
            ubiqos_hid_rearms++;
            continue;
        }

        // Ask the layer that owns the wire before asking the one that owns the
        // bookkeeping.
        //
        // tuh_hid_receive_ready reports the host stack's own record, and a
        // transfer that fails without ever completing leaves that record saying
        // "busy" for good -- so the sweep below, which only acts when the stack
        // admits to being idle, can never fire. That is precisely the state the
        // first ARM board sat in: nine keystrokes delivered, then endpoint 0x81
        // with three failures and NOTHING QUEUED, rearms and recoveries both
        // zero, and a replug that went unnoticed. PIO knows there is nothing on
        // the wire, and PIO is not guessing.
        //
        // The same fault exists on RISC-V and announces itself differently:
        // TU_ASSERT is an ebreak there, so it prints a stepped-over assertion,
        // where on ARM it returns false and says nothing -- ONLY when no
        // debugger is attached. TU_BREAKPOINT on ARM reads DHCSR and executes
        // BKPT #0 if C_DEBUGEN is set, so with a probe in the board the same
        // assertion HALTS THE CORE instead of returning. Measured 13 Sep 2026:
        // core 1 stopped at cdc_host.c:675 with DFSR reading BKPT, which also
        // froze TIMER0 through DBGPAUSE and left tusb_time_millis_api standing
        // still while core 0 ran on its own SysTick.
        //
        // So attaching a probe changes what this fault does, which is the worst
        // property an instrument can have. Do not conclude anything about USB
        // behaviour from a session with a probe attached without asking whether
        // the same run without one would have gone further.
        //
        // Aborting first is what makes the ask land. Without it the claim
        // inside tuh_hid_receive_report is refused for exactly the reason the
        // report is needed, and the retry would repeat for ever.
#if !UBIQOS_USB_NATIVE_HOST
        const endpoint_t *ep = hid_in_endpoint(hid_poll[i].addr, hid_poll[i].instance);
        if (ep && !ep->has_transfer) {
            if (++hid_poll[i].dead < HID_DEAD_SWEEPS) continue;
            hid_poll[i].dead = 0;
            if (hid_poll[i].tries >= HID_RECOVER_LIMIT) continue;
            hid_poll[i].tries++;
            tuh_edpt_abort_xfer(hid_poll[i].addr, ep->ep_num);
            hid_poll[i].armed = tuh_hid_receive_report(hid_poll[i].addr,
                                                       hid_poll[i].instance);
            hid_poll[i].idle = 0;
            ubiqos_hid_recoveries++;
            forget_held_keys();
            continue;
        }
#endif
        hid_poll[i].dead = 0;

        if (!tuh_hid_receive_ready(hid_poll[i].addr, hid_poll[i].instance)) {
            hid_poll[i].idle = 0;               // a transfer is out, as it should be
            continue;
        }
        if (++hid_poll[i].idle < HID_IDLE_SWEEPS) continue;

        hid_poll[i].idle = 0;
        hid_poll[i].armed = tuh_hid_receive_report(hid_poll[i].addr, hid_poll[i].instance);
        ubiqos_hid_recoveries++;
        forget_held_keys();
    }
}

void tuh_hid_mount_cb(uint8_t addr, uint8_t instance,
                      uint8_t const *desc, uint16_t len) {
    (void)desc; (void)len;
    uint8_t proto = tuh_hid_interface_protocol(addr, instance);
    ev_push(EV_LOG, proto == HID_ITF_PROTOCOL_KEYBOARD ? LOG_HID_KBD : LOG_HID_OTHER, 0, 0);

    // Nothing is held on a keyboard that has just arrived. Saying so costs one
    // call and closes the half of the runaway that a lost umount leaves open:
    // a keyboard that re-enumerates without one would otherwise inherit the
    // key the previous instance was believed to be holding, and restart the
    // five-second clock with it every time round.
    forget_held_keys();

    hid_want(addr, instance);

    // Said after the ask and not before it, because the boot has stopped
    // between these two lines and there was no way to tell which side of
    // hid_want it was on: the first report being asked for, or TinyUSB
    // enumerating the next interface. Seen 6 Sep 2026, intermittently, on a
    // keyboard that presents two HID interfaces. One line at boot is cheap
    // against a hang that only shows up sometimes.
    ev_push(EV_LOG, LOG_ARMED, 0, 0);
}

void tuh_hid_umount_cb(uint8_t addr, uint8_t instance) {
    for (int i = 0; i < HID_SLOTS; i++) {
        if (hid_poll[i].wanted && hid_poll[i].addr == addr && hid_poll[i].instance == instance) {
            hid_poll[i].wanted = false;
            hid_poll[i].armed = false;
        }
    }

    // And a keyboard that has gone is holding nothing down.
    //
    // This was missing, and it is the second half of "key repeat outlives the
    // keyboard". The clock has no way of knowing a key was let go -- a held key
    // produces no reports at all -- so the release report is normally the only
    // thing that ends a repeat. A keyboard that vanishes never sends one.
    //
    // REPEAT_LIMIT_MS bounded the damage at five seconds, which at 35 ms a
    // repeat is still about a hundred and forty phantom keystrokes, and the key
    // in question is Return because pressing Return is how the command that
    // lost the keyboard was started. The shell obeys every one of them. That
    // flood is what fed the video pump enough work to stall the board against
    // the QMI on 11 Sep 2026 -- see docs/the-board-stopped.md, where this is
    // the trigger and the stall is the consequence.
    //
    // Counted as well as cleared: a repeat that was in flight when its keyboard
    // left is the exact event nobody could see, and one number would have named
    // this fault in an afternoon rather than over two days.
    if (repeat_key) ubiqos_hid_lost_repeats++;
    forget_held_keys();

    ubiqos_print("USB host: HID gone\n");
}

// The layout comes from the keyboard's descriptor, which is where it belongs:
// changing it is a matter of replacing one module rather than rebuilding the
// kernel. Until a descriptor has been registered these two lines stand in --
// enough to type a command, and American whatever is printed on the keys.
static const ubiqos_keymap_t *keymap;

void ubiqos_usbhost_set_keymap(const ubiqos_keymap_t *k) { keymap = k; }

// Auto-repeat. When a held key first stopped flooding the queue it stopped
// repeating altogether, which is the other half of the problem: a report says
// which keys are DOWN and arrives on every poll, so emitting all of them gave
// "hhjjjkkkk", and emitting only the new ones gives no repeat at all. The answer
// is neither -- it is a clock.
//
// The repeat runs from the USB task rather than from the report callback,
// because a keyboard with nothing to say answers a poll with NAK and no
// callback arrives. Waiting for one would mean a key that repeats only while
// some other key is being pressed.
#define REPEAT_DELAY_MS 400      // before the first repeat
#define REPEAT_RATE_MS   35      // between them after that

// And a limit, because the clock has no way of knowing the key was let go.
//
// A held key produces no reports at all -- the keyboard answers each poll with
// a NAK until something changes -- so silence cannot be told from a keyboard
// that has stopped answering. That leaves the release report as the only thing
// that ends a repeat, and losing one costs the machine: Return repeated for
// ever, an empty command each time, the prompt redrawn over the bottom row
// until the power is cut. It happened repeatedly on 3 Sep 2026.
//
// Five seconds is longer than anyone holds a key on purpose except on the
// cursor keys, and there the cost of the limit is lifting a finger and pressing
// again. Against that: a lost report costs one keystroke instead of the
// session. The trade is not close.
#define REPEAT_LIMIT_MS 5000

// repeat_key itself is declared near the top of this file, because the HID
// mount and umount callbacks are above this point and both have to clear it.
static uint8_t  repeat_mods;
static uint32_t repeat_due;
static uint32_t repeat_began;    // when this key started repeating

// The keys the previous report said were down. At file scope rather than inside
// the callback because a lost transfer has to be able to clear it: see below.
static uint8_t  was[6];

// Everything this side believes about which keys are held, dropped.
//
// A key that was down when the reports stopped coming is not down now, and if it
// still is, the next report will say so. Repeating it in the meantime turns one
// lost transfer into an unbroken stream -- and the key that was down was Return,
// because pressing Return is how the command that lost the report was started.
// The shell obeyed it: an empty line, a prompt, for ever, over the same bottom
// row of the screen. From the outside that is a dead keyboard with a flickering
// prompt, and neither half looks like a missing USB transfer.
static void forget_held_keys(void) {
    repeat_key = 0;
    for (int i = 0; i < 6; i++) was[i] = 0;
}

static uint8_t translate(uint8_t k, uint8_t mods);

// Ctrl-C never reaches the queue while something is running in front of the
// screen: it ends that process instead. With nothing running it goes through as
// an ordinary character, because then there is a shell reading and it can do
// something better with it -- clearing the line -- than the kernel can.
bool ubiqos_io_interrupt(const char *device_name);

void ubiqos_usbhost_push_str(const char *sq);   // defined below

// The keymap holds one byte per key and cannot hold anything else: a layout is
// a table of characters, and a-ring is one character. What leaves here is UTF-8,
// because that is what the machine reads and writes -- everything from U+0080
// to U+00FF is two bytes, so a-ring goes out as two.
//
// Both halves or neither. A lone lead byte in the queue would be read as a
// broken character, and there is no way to take it back.
static void emit(uint8_t c) {
    // Whether an interrupt goes to a process or falls through as a character
    // is a question about the kernel's process table, which is core 0's to
    // answer. So the key is reported and core 0 decides -- and pushes ^C into
    // the ring itself if nothing wanted it.
    if (c == 3) { ev_push(EV_INTR, 0, 0, 0); return; }
    if (c < 0x80) { push_locked((const char *)&c, 1); return; }

    char pair[3];
    pair[0] = (char)(0xc0 | (c >> 6));
    pair[1] = (char)(0x80 | (c & 0x3f));
    pair[2] = 0;
    ubiqos_usbhost_push_str(pair);
}

// The arrows and their neighbours, as the escape sequences every terminal has
// sent for them since the VT100. They are not in the keymap and should not be:
// a layout says which letter is on a key, and an arrow is an arrow on every
// keyboard in the world. Putting them here also means the serial port and the
// screen deliver the same bytes for the same key, which is what lets the shell
// have one idea of how to edit a line rather than two.
//
// HID usages: 0x4a Home, 0x4b PgUp, 0x4c Delete, 0x4d End, 0x4e PgDn,
// 0x4f right, 0x50 left, 0x51 down, 0x52 up.
// Scrolling the screen back, with nothing but a keyboard.
//
// Shift and the navigation keys, which is what a Linux virtual console does and
// for the same reason: an UNSHIFTED PageUp has to go on reaching the program --
// an editor wants it -- while shift is free in every layout. So the modifier is
// what separates "scroll the terminal" from "page up in what I am running", and
// no application can lose a key to this.
//
// Shift+PgUp and Shift+PgDn move half a screen, Shift+Home goes as far back as
// there is, Shift+End returns to the live screen.
static bool scroll_key(uint8_t k, uint8_t mods) {
#if UBIQOS_VIDEO_CHARGEN
    if (!(mods & 0x22))                       // either shift
        return false;
    switch (k) {
    case 0x4b: ev_push(EV_VIEW, VIEW_MOVE, (uint32_t)(-(int32_t)(UBIQOS_CELL_ROWS / 2)), 0); return true;
    case 0x4e: ev_push(EV_VIEW, VIEW_MOVE, (uint32_t)(int32_t)(UBIQOS_CELL_ROWS / 2), 0); return true;
    case 0x4a: ev_push(EV_VIEW, VIEW_HOME, 0, 0); return true;
    case 0x4d: ev_push(EV_VIEW, VIEW_END, 0, 0);  return true;
    default:   return false;
    }
#else
    (void)k; (void)mods;
    return false;                             // a bitmap has no history to show
#endif
}

// Anything that is going to be read as input puts the screen back where the
// cursor is. Output does not: a line printed while you are reading history
// should not yank the page away, and that difference is the whole of the rule.
static void scroll_to_live(void) {
#if UBIQOS_VIDEO_CHARGEN
    ev_push(EV_VIEW, VIEW_END, 0, 0);
#endif
}

static const char *nav_sequence(uint8_t k) {
    switch (k) {
    case 0x4a: return "\x1b[H";
    case 0x4b: return "\x1b[5~";
    case 0x4c: return "\x1b[3~";
    case 0x4d: return "\x1b[F";
    case 0x4e: return "\x1b[6~";
    case 0x4f: return "\x1b[C";
    case 0x50: return "\x1b[D";
    case 0x51: return "\x1b[B";
    case 0x52: return "\x1b[A";
    default:   return 0;
    }
}

// A whole sequence or none of it. Half an escape sequence in the queue is worse
// than a dropped keystroke: the reader would take the tail for text.
//
// Not static: the console answers a cursor-position report through here. That
// looks like a strange direction until you remember what a terminal is -- the
// screen's reply to the program goes to the program's input, and on this
// machine the console's input is the keyboard.
void ubiqos_usbhost_push_str(const char *sq) {
    uint32_t n = 0;
    while (sq[n]) n++;
    push_locked(sq, n);
}

void ubiqos_usbhost_repeat(void) {
    if (!repeat_key) return;
    uint32_t now = tusb_time_millis_api();
    if ((int32_t)(now - repeat_began) > REPEAT_LIMIT_MS) { forget_held_keys(); return; }
    if ((int32_t)(now - repeat_due) < 0) return;
    const char *sq = nav_sequence(repeat_key);
    if (sq) {
        ubiqos_usbhost_push_str(sq);
    } else {
        uint8_t c = translate(repeat_key, repeat_mods);
        if (c) emit(c);
    }
    repeat_due = now + REPEAT_RATE_MS;
}

// Return gives a carriage return, as every terminal has since the teletype --
// not a line feed. The shell takes either and so never noticed, but cu passes
// bytes through untouched, and the device at the other end may well care: a
// BleuIO ends its commands on CR and answers nothing at all to a line feed.
static const char plain[] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\\0;'`,./";
static const char shift[] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|\0:\"~<>?";

// One keycode and the modifier byte to a character, through whichever layout is
// in force. Shared by the first press and by every repeat after it.
// Control turns a letter into the code it has stood for since teletypes: ctrl-A
// is 1, ctrl-L is 12. It is applied after the layout rather than through it, so
// it works whatever letter the key carries and needs no entry in the keymap --
// and only to letters, because ctrl with anything else has no agreed meaning
// worth inventing one for.
static uint8_t control(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 1);
    return 0;
}

static uint8_t translate(uint8_t k, uint8_t mods) {
    bool sh   = (mods & 0x22) != 0;             // either shift
    bool alt  = (mods & 0x40) != 0;             // right alt, which is AltGr
    bool ctrl = (mods & 0x11) != 0;             // either control

    if (keymap) {
        if (k >= UBIQOS_KEYMAP_KEYS) return 0;
        uint8_t c = alt ? keymap->altgr[k] : (sh ? keymap->shift[k] : keymap->plain[k]);
        // AltGr on a key with nothing there falls back to the unshifted
        // character, as it does everywhere else.
        if (alt && !c) c = keymap->plain[k];
        return ctrl ? control(c) : c;
    }
    if (k < sizeof(plain) - 1) {
        uint8_t c = (uint8_t)(sh ? shift[k] : plain[k]);
        return ctrl ? control(c) : c;
    }
    return 0;
}

// --- CDC-ACM ON THE HOST SIDE ---------------------------------------------
// The serial port that is not a serial port: a BLE dongle, a modem, a sensor.
// TinyUSB's class driver does the protocol; what is kept here is which
// interface index turned up, because the UbiqOS device that a process opens has
// to point at something.
//
// One at a time. The class is configured for one interface and a second would
// need a device name of its own to be reachable, which is a descriptor question
// rather than a driver one.
static int32_t cdc_index = -1;

int32_t ubiqos_usbhost_cdc_index(void) { return cdc_index; }

void tuh_cdc_mount_cb(uint8_t idx) {
    cdc_index = (int32_t)idx;

    // Say which device, because "it is connected but does not answer" is a
    // question about what the device is, and nothing else here can answer it.
    tuh_itf_info_t info;
    uint16_t vid = 0, pid = 0;
    if (tuh_cdc_itf_get_info(idx, &info))
        tuh_vid_pid_get(info.daddr, &vid, &pid);

    ubiqos_print("USB host: CDC-ACM ready as 'acm', ");
    ubiqos_print_hex(vid);
    ubiqos_print(":");
    ubiqos_print_hex(pid);
    ubiqos_print(tuh_cdc_get_dtr(idx) ? ", DTR high\n" : ", DTR low\n");
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (cdc_index == (int32_t)idx) cdc_index = -1;
    ubiqos_print("USB host: CDC-ACM device gone\n");
}

void tuh_hid_report_received_cb(uint8_t addr, uint8_t instance,
                                uint8_t const *report, uint16_t len) {
    // A report arriving is the proof that recovery worked, so the budget is
    // refilled here rather than counted down to nothing. A keyboard that keeps
    // coming back is never rationed; only one that never answers runs out.
    for (int i = 0; i < HID_SLOTS; i++) {
        if (hid_poll[i].wanted && hid_poll[i].addr == addr
                               && hid_poll[i].instance == instance) {
            hid_poll[i].tries = 0;
            hid_poll[i].dead = 0;
            break;
        }
    }

    if (len >= 8 && tuh_hid_interface_protocol(addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        // A report lists the keys that are DOWN, not the ones just pressed, and
        // it arrives on every poll. Emitting all of them each time turned one
        // held key into a stream of them -- "hhjjjkkkk" for three keystrokes.
        // So each report is compared with the one before, and only keys that
        // were not already down produce a character. 'was' is at file scope so
        // that a lost transfer can clear it; see forget_held_keys.

        for (int i = 2; i < 8; i++) {
            uint8_t k = report[i];
            if (!k) continue;

            bool held = false;
            for (int j = 0; j < 6; j++) if (was[j] == k) held = true;
            if (held) continue;

            if (scroll_key(k, report[0])) {
                // Not input, and deliberately not set up to repeat: holding it
                // would page straight past what you are trying to read.
                continue;
            }

            const char *sq = nav_sequence(k);
            if (sq) {
                scroll_to_live();
                ubiqos_usbhost_push_str(sq);
            } else {
                uint8_t c = translate(k, report[0]);
                if (c) { scroll_to_live(); emit(c); }
            }

            // The newest key down is the one that repeats, as it is everywhere:
            // hold a, then hold b, and it is b that runs away.
            repeat_key   = k;
            repeat_mods  = report[0];
            repeat_began = tusb_time_millis_api();
            repeat_due   = repeat_began + REPEAT_DELAY_MS;
        }
        // A key that has been let go stops repeating. Modifiers are taken
        // afresh, so shift released mid-repeat turns capitals into small ones.
        if (repeat_key) {
            bool still = false;
            for (int i = 2; i < 8; i++) if (report[i] == repeat_key) still = true;
            if (!still) repeat_key = 0;
            else repeat_mods = report[0];
        }

        for (int i = 0; i < 6; i++) was[i] = report[i + 2];
    }
    hid_want(addr, instance);
}

// --- WHAT THE HOST STACK THINKS, WITHOUT A PROBE --------------------------
//
// Every number below was read with J-Link on 3 Sep 2026, and reading them that
// way is what taught us not to. Memory access on Hazard3 goes through a halt,
// and PIO-USB bit-bangs a bus whose timing is measured in microseconds: each
// mem8 stopped the processor long enough to lose transactions, so the probe
// manufactured the very failures it was brought in to observe. Hours went into
// symptoms that were mine.
//
// So the same state is reported here, over the UART or the CDC console, by a
// machine that never stops. One word at a time, because a syscall that returns
// a structure would have to copy it into the caller's memory and this needs no
// such ceremony -- usbstat asks for the fields it wants and lays them out.
uint32_t ubiqos_usbhost_info(uint32_t what) {
    if (what == UBIQOS_USB_REARMS)     return ubiqos_hid_rearms;
    if (what == UBIQOS_USB_RECOVERIES) return ubiqos_hid_recoveries;
    if (what == UBIQOS_USB_CDCREARMS)  return ubiqos_cdc_rearms;
    if (what == UBIQOS_USB_CDCGIVEUP)  return ubiqos_cdc_gaveup;
    if (what == UBIQOS_USB_REPEATKEY)  return repeat_key;
    if (what == UBIQOS_USB_KEYSIN)     return head;
    if (what == UBIQOS_USB_CORE1_BEATS) return core1_beats;
    if (what == UBIQOS_USB_CORE1_AGE)   return tusb_time_millis_api() - core1_last_ms;

    // The device side's, not this file's -- but usbstat asks one question of
    // one call, and splitting it in two for four counters would be ceremony.
    {
        extern uint32_t ubiqos_usb_suspends, ubiqos_usb_resumes;
        extern uint32_t ubiqos_usb_mounts, ubiqos_usb_unmounts;
        extern uint32_t ubiqos_usb_last_event_ms;
        if (what == UBIQOS_USB_SUSPENDS)  return ubiqos_usb_suspends;
        if (what == UBIQOS_USB_RESUMES)   return ubiqos_usb_resumes;
        if (what == UBIQOS_USB_MOUNTS)    return ubiqos_usb_mounts;
        if (what == UBIQOS_USB_UNMOUNTS)  return ubiqos_usb_unmounts;
        if (what == UBIQOS_USB_LASTEVENT) return ubiqos_usb_last_event_ms;
    }

#if !UBIQOS_USB_NATIVE_HOST             // see the note above the endpoint pool
    if (what == UBIQOS_USB_ROOT) {
        const root_port_t *r = PIO_USB_ROOT_PORT(0);
        return (uint32_t)r->initialized | ((uint32_t)r->connected << 1)
             | ((uint32_t)r->is_fullspeed << 2) | ((uint32_t)r->suspended << 3)
             | ((uint32_t)r->event << 8);
    }
#endif

    if (what >= UBIQOS_USB_HID && what < UBIQOS_USB_HID + HID_SLOTS) {
        int i = (int)(what - UBIQOS_USB_HID);
        return (uint32_t)hid_poll[i].addr | ((uint32_t)hid_poll[i].instance << 8)
             | ((uint32_t)hid_poll[i].wanted << 16) | ((uint32_t)hid_poll[i].armed << 17)
             | ((uint32_t)hid_poll[i].idle << 24);
    }

    // The root port and the endpoint pool are PIO-USB's. A host on the chip's
    // controller answers zero for both, which usbstat shows as nothing there.
#if !UBIQOS_USB_NATIVE_HOST
    if (what >= UBIQOS_USB_EP && what < UBIQOS_USB_EP + PIO_USB_EP_POOL_CNT) {
        const endpoint_t *e = PIO_USB_ENDPOINT((int)(what - UBIQOS_USB_EP));
        // Bit 19 says the slot is in use. Closing an endpoint sets size to zero
        // and leaves dev_addr and ep_num where they were -- "ep size is used as
        // valid indicator", says the library, and allocation reuses any slot
        // whose size is zero. Without this a listing shows every slot that has
        // ever been used, still wearing the name of whatever last had it.
        //
        // Bit 20 says the address belongs to a hub. TinyUSB numbers hubs from
        // CFG_TUH_DEVICE_MAX + 1, so the first one is device 6 here -- which
        // looked like a ghost for a whole afternoon and is the most ordinary
        // thing on the board. Its two endpoints are what every hub has: a
        // control pipe, and a one-byte interrupt IN carrying a bit per port.
        return (uint32_t)e->dev_addr | ((uint32_t)e->ep_num << 8)
             | ((uint32_t)e->has_transfer << 16) | ((uint32_t)e->transfer_started << 17)
             | ((uint32_t)e->stalled << 18) | ((uint32_t)(e->size != 0) << 19)
             | ((uint32_t)(e->dev_addr > CFG_TUH_DEVICE_MAX) << 20)
             | ((uint32_t)e->failed_count << 24);
    }
#endif
    return 0;
}
