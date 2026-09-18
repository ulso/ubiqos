// The Waveshare RP2350-Touch-LCD-4.3B, as UbiqOS needs to know it.
//
// The second board, and the first that is not the Fruit Jam. Same chip --
// RP2350B, so the kernel, both architectures, the scheduler and the module
// loader are unchanged -- and almost everything around it is different.
//
// What this board has not got is as important as what it has, and each absence
// below is a thing the kernel must be built without rather than merely not use.
#ifndef UBIQOS_BOARD_WS43B_H
#define UBIQOS_BOARD_WS43B_H

#define UBIQOS_BOARD_NAME   "Waveshare RP2350-Touch-LCD-4.3B"
#define UBIQOS_PIN_COUNT    48u              // RP2350B

// --- NO DIAGNOSTIC UART ----------------------------------------------------
// The only pins brought out to pads are GPIO0 and GPIO1, the UART0 pair, and
// those are the PIO USB host. So there is no serial console on this board at
// all, and that is a deliberate trade rather than an oversight.
//
// It has to be a build-time absence, not an unused peripheral: ubiqos_putc
// WAITS on the UART, so an uninitialised one would hang the kernel on its first
// line. What is left is the screen, the USB console, and the dmesg ring read
// out over the debug probe -- which is how the first boot here was watched.
#define UBIQOS_HAS_DIAG_UART    0

// --- NO VIDEO YET ----------------------------------------------------------
// The panel is 800x480 RGB565 behind an ST7262, driven by PIO -- nothing like
// DVI out of HSTX, so kernel/video.c does not apply and there is no driver for
// this one yet. UBIQOS_VIDEO=none builds without video, chargen and console.
//
//   DE 20, VSYNC 21, HSYNC 22, PCLK 23, data D0..D15 on 24..39 unbroken,
//   RST 19, EN 18, backlight 40, pixel clock 20 MHz.
//
// Written down here because the numbers were the hard part to find, and the
// unbroken run of sixteen data pins is what will let PIO shift a pixel out in
// one instruction.
#define UBIQOS_LCD_DE_PIN       20
#define UBIQOS_LCD_VSYNC_PIN    21
#define UBIQOS_LCD_HSYNC_PIN    22
#define UBIQOS_LCD_PCLK_PIN     23
#define UBIQOS_LCD_DATA0_PIN    24           // through 39, RGB565
#define UBIQOS_LCD_RST_PIN      19
#define UBIQOS_LCD_EN_PIN       18
#define UBIQOS_LCD_BL_PIN       40

// Where the backlight starts, as a percentage. Not a hundred: this panel at
// full brightness lights a room in the evening, and bright light is disabling
// rather than merely unpleasant for some eyes -- cone dystrophy among them. A
// screen nobody can look at shows nothing, so the comfortable setting is the
// default and the bright one is asked for.
//
// Twenty, and it was tried rather than reasoned. 35 was a guess; 10 was too
// dark and 20 was right, judged on the bench against a dark screen in an
// ordinary room. It belongs with the palette: light figures on a near-black
// ground need far less backlight than the white panel this replaced.
#define UBIQOS_BACKLIGHT_DEFAULT 20u
#define UBIQOS_LCD_PCLK_HZ      20000000u

// --- TOUCH -----------------------------------------------------------------
// A GT911 on I2C1, five points, with an interrupt. The RTC is on the same bus.
#define UBIQOS_TOUCH_INT_PIN    16
#define UBIQOS_TOUCH_RST_PIN    17
#define UBIQOS_TOUCH_I2C_ADDR   0x5du
#define UBIQOS_RTC_I2C_ADDR     0x51u        // PCF85063

// --- USB HOST ON PIO -------------------------------------------------------
// The RX/TX pads, with a USB-A socket soldered on. GPIO0 and GPIO1 are
// adjacent with the lower first, which is PIO_USB_PINOUT_DPDM and the same
// convention the Fruit Jam uses.
//
// The 5V is wired permanently, so unlike the Fruit Jam there is no way to
// power-cycle the port from software. That matters: on the other board a
// power cycle was more than once the only thing that recovered a wedged
// device.
// PIO-USB takes three state machines of pio0 and nothing else -- its defaults
// are TX and RX both on index 0 -- so the budget closes exactly: pio0 the host,
// pio1 the panel's sync, pio2 its pixels. All three blocks, nothing spare.
//
// pio0 keeps the default GPIO base of 0, which is what D+ on GPIO0 needs; the
// panel's two blocks are moved to 16.
//
// Two things in videorgb.c follow from the host being here: that GPIO base, and
// leaving DMA channel 0 alone, which the library claims by number.
//
// Built with UBIQOS_NATIVE_USB=host none of this is used: the chip's own USB
// socket is the host, and the pads and pio0 are left alone.
#if UBIQOS_USB_NATIVE_HOST
#define UBIQOS_HAS_PIO_USB_HOST 0
#define UBIQOS_USB_HOST_PADS
#else
#define UBIQOS_HAS_PIO_USB_HOST 1
#define UBIQOS_USB_HOST_PADS        { 0, "usb host" }, { 1, "usb host" },
#endif
#define UBIQOS_USB_HOST_DP          0        // D- is GPIO1
#define UBIQOS_HAS_USB_HOST_POWER   0

// --- THE CARD ---------------------------------------------------------------
//
// A microSD slot on four data lines, from Waveshare's own pin table: SDIO_SCK
// on GPIO10, SDIO_CMD on 11, and D0 to D3 on 12 through 15. The four data pins
// are CONSECUTIVE from D0, which is what the driver requires -- it is given the
// first and counts, so a board that scattered them could only be read one bit
// at a time.
//
// The pins go to CMake rather than into this header, because the driver they
// configure is third-party code that has never heard of a UbiqOS board header:
// it reads PICO_SD_* as compile definitions and nothing else.
#define UBIQOS_HAS_SD 1

// --- WHAT IS ABSENT --------------------------------------------------------
// No ESP32-C6, so no ESP-Hosted and no radio: lwIP here can only ever be the
// USB cable. Nothing holds a hub, a DAC and a radio in reset either, so there
// is no peripheral reset to release.
#define UBIQOS_HAS_ESP_HOSTED   0
#define UBIQOS_HAS_PERIPH_RESET 0

// --- WHAT THE HARDWARE FIXES -----------------------------------------------
// The LCD's twenty pins are the board rather than a driver's choice, so they are
// claimed at boot. The USB host pair is claimed before there is a driver for it,
// because a person asking for GPIO0 at a shell should be told what has it.
//
// The touch controller's four -- the I2C pair and its interrupt and reset -- are
// NOT here, and that is the rule this file nearly broke: a driver's pins are
// claimed by the driver, at the moment it configures itself, so the table says
// what is true of this boot rather than of the board. Listing them here made the
// gt911 driver's own claim fail and print "a pin was already claimed", which is
// exactly the collision report the table exists to give and was, this once, a
// lie.
#define UBIQOS_BOARD_FIXED_PINS                                              \
    UBIQOS_USB_HOST_PADS                                                      \
    { 10, "sd" }, { 11, "sd" }, { 12, "sd" }, { 13, "sd" },                   \
    { 14, "sd" }, { 15, "sd" },                                               \
    { 18, "lcd" }, { 19, "lcd" }, { 20, "lcd" }, { 21, "lcd" },               \
    { 22, "lcd" }, { 23, "lcd" }, { 24, "lcd" }, { 25, "lcd" },               \
    { 26, "lcd" }, { 27, "lcd" }, { 28, "lcd" }, { 29, "lcd" },               \
    { 30, "lcd" }, { 31, "lcd" }, { 32, "lcd" }, { 33, "lcd" },               \
    { 34, "lcd" }, { 35, "lcd" }, { 36, "lcd" }, { 37, "lcd" },               \
    { 38, "lcd" }, { 39, "lcd" }, { 40, "lcd backlight" },                    \
    { 47, "psram" },

#endif
