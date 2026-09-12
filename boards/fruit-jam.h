// The Adafruit Fruit Jam, as myrtos needs to know it.
//
// One file per board, and this is the first. Everything here was a #define
// scattered through kernel/main.c, kernel/usbhost.c, kernel/video.c and
// kernel/pins.c -- five files that each knew a little about one particular
// machine. Gathering it changes nothing on this board and is what makes a
// second one possible.
//
// What belongs here: a number or a fact that changes when the BOARD changes.
// What does not: anything a driver owns. A driver's pins arrive in its
// descriptor, which is a data module and therefore already per-board -- see
// modules/*_desc. That split is older than this file and it is the right one.
#ifndef MYRTOS_BOARD_FRUIT_JAM_H
#define MYRTOS_BOARD_FRUIT_JAM_H

#define MYRTOS_BOARD_NAME   "Adafruit Fruit Jam"
#define MYRTOS_PIN_COUNT    48u              // RP2350B

// --- DIAGNOSTICS -----------------------------------------------------------
// The chip's UART0 would rather be on GP8/GP9, but those go to the on-board
// ESP32-C6 rather than to any header, so the diagnostic line is GP44 -- which
// is UART0's TX there, hence uart0 and not the SDK header's default of uart1.
#define MYRTOS_HAS_DIAG_UART 1
#define MYRTOS_UART          uart0
#define MYRTOS_UART_TX_PIN   44

// --- USB HOST ON PIO -------------------------------------------------------
// D- is D+ plus one, which is PIO_USB_PINOUT_DPDM and what Pico-PIO-USB
// defaults to. The two must be adjacent whichever order is chosen.
#define MYRTOS_HAS_PIO_USB_HOST 1
#define MYRTOS_USB_HOST_DP          1
#define MYRTOS_HAS_USB_HOST_POWER   1
#define MYRTOS_USB_HOST_POWER       11       // switched, so a port can be cycled

// --- WHAT A RESET RELEASES -------------------------------------------------
// One pin holds the USB hub, the audio DAC and the ESP32-C6 together, and the
// ESP samples its GPIO9 as it leaves reset -- which is wired to GP0 here, so
// the strap has to be high before the reset is released. See usbhost.c.
#define MYRTOS_HAS_PERIPH_RESET 1
#define MYRTOS_PERIPH_RESET     22
#define MYRTOS_ESP_BOOT_STRAP   0

// --- VIDEO -----------------------------------------------------------------
// DVI out of the HSTX peripheral: GP12 CKN, GP13 CKP, then D0, D1, D2 as N,P
// pairs, so one unbroken run.
#define MYRTOS_VIDEO_DVI_HSTX   1
#define MYRTOS_VIDEO_PIN_FIRST  12
#define MYRTOS_VIDEO_PIN_LAST   19

// --- THE RADIO -------------------------------------------------------------
// An ESP32-C6 running ESP-Hosted, over SPI.
//
// NOTHING READS THIS YET, and it is here anyway because it is the fact a second
// board will differ by first: one without a radio must build without
// kernel/ehnet.c and the ehspi driver, with lwIP left as only the USB cable.
// Making that conditional is work, and work with no second board to test it
// against is work done blind -- so the fact is written down and the plumbing
// waits for something that needs it.
#define MYRTOS_HAS_ESP_HOSTED   1

// --- WHAT THE HARDWARE FIXES -----------------------------------------------
// Claimed at boot so nothing has to discover it the hard way. A driver's own
// pins are NOT here: those are claimed when the driver configures itself, so
// the table says what is true of this boot rather than of the board.
//
// The buttons are deliberately absent. Nothing owns them -- they are switches
// on pins, and a program that wants to read one should be able to. Ownership is
// about who would break if the pin moved under them.
#define MYRTOS_BOARD_FIXED_PINS                                              \
    { 1, "usb host" }, { 2, "usb host" }, { 11, "usb 5V" },                   \
    { 3, "wifi" }, { 22, "wifi reset" }, { 28, "wifi" }, { 30, "wifi" },      \
    { 31, "wifi" }, { 46, "wifi" },                                           \
    { 12, "video" }, { 13, "video" }, { 14, "video" }, { 15, "video" },       \
    { 16, "video" }, { 17, "video" }, { 18, "video" }, { 19, "video" },       \
    { 33, "sd card" }, { 34, "sd card" }, { 35, "sd card" }, { 36, "sd card" },\
    { 37, "sd card" }, { 38, "sd card" }, { 39, "sd card" },                  \
    /* The kernel's own debug UART, which had never been written down until   \
       the table existed. It is also the board's A4, so the ADC asks for it   \
       and is refused -- which is the whole point: the collision is reported   \
       instead of the console quietly going dead. */                          \
    { MYRTOS_UART_TX_PIN, "uart" },

#endif
