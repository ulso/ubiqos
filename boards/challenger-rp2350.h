// iLabs' Challenger+ RP2350 WiFi6/BLE5, as UbiqOS needs to know it.
//
// A Feather: an RP2350A with 8 MB of flash and 8 MB of PSRAM, an ESP32-C6
// beside it, a LiPo charger, and a USB-C socket that is the chip's own device
// port. Nearly a Fruit Jam with the video, the card and the USB host taken
// away -- the radio is the same chip on the same kind of link, and the kernel
// drives it the same way.
//
// Every pin here is from iLabs' schematic, 54-00314-1 rev P1.2, and agrees with
// the SDK's boards/ilabs_challenger_rp2350_wifi_ble.h where both say something.
// The contract a board header owes is written out in boards/fruit-jam.h.
#ifndef UBIQOS_BOARD_CHALLENGER_RP2350_H
#define UBIQOS_BOARD_CHALLENGER_RP2350_H

#define UBIQOS_BOARD_NAME   "iLabs Challenger+ RP2350 WiFi6/BLE5"

// There is no card to name it, so it names itself, as the Nucleo boards do.
#define UBIQOS_DEFAULT_HOSTNAME "challenger"

// And its cable's subnet, 192.168.8.x rather than the Fruit Jam's 192.168.7.x:
// with both on one computer, two boards at 192.168.7.1 gave it one subnet on
// two cables, and the Challenger's never came up.
#define UBIQOS_DEFAULT_USB_ADDRESS ((192u << 24) | (168u << 16) | (8u << 8) | 1u)
#define UBIQOS_PIN_COUNT    30u              // RP2350A

// --- DIAGNOSTICS -----------------------------------------------------------
// UART0 on the header's TX and RX, GP12 and GP13 -- the SDK's default UART for
// this board. Nothing is there unless somebody connects it; the USB console is
// the one that is always there.
#define UBIQOS_HAS_DIAG_UART 1
#define UBIQOS_UART          uart0
#define UBIQOS_UART_TX_PIN   12

// --- USB HOST --------------------------------------------------------------
// None. The USB-C socket is the chip's device port: the console and the
// network over the cable, as on the Fruit Jam.
#define UBIQOS_HAS_PIO_USB_HOST 0

// --- WHAT A RESET RELEASES -------------------------------------------------
// The ESP32-C6's EN is GP15, its own and nobody else's. Its IO9 is GP14 -- the
// boot strap as it leaves reset, DATA READY after -- and must be high as the
// reset is released or the chip waits in its serial bootloader.
//
// iLabs' schematic adds one more: "make sure the signal ESP32_MISO is kept high
// during the release of reset to avoid ending up in an unknown state." That is
// GP8 here, an input on this side, so it is pulled up while the reset goes.
#define UBIQOS_HAS_PERIPH_RESET 1
#define UBIQOS_PERIPH_RESET     15
#define UBIQOS_ESP_BOOT_STRAP   14
#define UBIQOS_ESP_RESET_PULLUP 8

// --- VIDEO -----------------------------------------------------------------
// None: UBIQOS_VIDEO=none, and no pins.

// --- THE RADIO -------------------------------------------------------------
// The ESP32-C6-MINI-1 running ESP-Hosted over SPI1. Its pins are the driver's
// and arrive in its descriptors -- modules/ehspi/eh_desc_challenger.c and
// modules/esplink/esp_desc_challenger.c. Shipped with Espressif's AT firmware,
// which carries its own IP stack; UbiqOS needs ESP-Hosted on it instead.
#define UBIQOS_HAS_ESP_HOSTED   1

// --- FLASH -----------------------------------------------------------------
// 8 MB, where the Fruit Jam has 16: past the chip's size the flash window wraps
// round, so the Fruit Jam's layout would put the application over the kernel
// and the key store in the middle of the modules. Modules from a megabyte in,
// as everywhere; the application after them; the key store after that, clear
// of the last sector -- which is where a UF2's RP2350-E10 block lands, at
// 0x10ffff00, which on this chip is 0x107fff00. See kernel/flashmod.h.
#define UBIQOS_BOARD_FLASH_APP_BASE    0x10600000u
#define UBIQOS_BOARD_FLASH_END         0x107E0000u
#define UBIQOS_BOARD_FLASH_KEYS_BASE   0x107E0000u
#define UBIQOS_BOARD_FLASH_KEYS_SIZE   0x2000u

// --- THE KEY STORE ---------------------------------------------------------
// Open by default, as on the NUCLEO-H563ZI: there is no card to keep a key file
// on, and a board that has to join its network by itself after every restart
// -- a bridge on a battery -- cannot wait for somebody to type `key unlock`.
// What that costs is the same: the store keeps its secrets from nobody who can
// read the flash. See UBIQOS_KEYS_DEFAULT_PASS in kernel/fsserver.c; a store
// sealed under a passphrase of its own stays shut until it is typed.
#define UBIQOS_KEYS_OPEN_BY_DEFAULT 1

// --- WHAT THE HARDWARE FIXES -----------------------------------------------
#define UBIQOS_BOARD_FIXED_PINS                                              \
    /* GP4 and GP5, the C6's serial line, are esplink's and claimed by it --  \
       listed here, they were refused to the one driver that uses them. */    \
    { 0, "psram" },                                                           \
    { 8, "wifi" }, { 9, "wifi" }, { 10, "wifi" }, { 11, "wifi" },             \
    { 14, "wifi" }, { 15, "wifi reset" }, { 22, "wifi" },                     \
    { UBIQOS_UART_TX_PIN, "uart" },

#endif
