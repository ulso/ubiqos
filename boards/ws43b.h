// The Waveshare RP2350-Touch-LCD-4.3B, as myrtos needs to know it.
//
// The second board, and the first that is not the Fruit Jam. Same chip --
// RP2350B, so the kernel, both architectures, the scheduler and the module
// loader are unchanged -- and almost everything around it is different.
//
// What this board has not got is as important as what it has, and each absence
// below is a thing the kernel must be built without rather than merely not use.
#ifndef MYRTOS_BOARD_WS43B_H
#define MYRTOS_BOARD_WS43B_H

#define MYRTOS_BOARD_NAME   "Waveshare RP2350-Touch-LCD-4.3B"
#define MYRTOS_PIN_COUNT    48u              // RP2350B

// --- NO DIAGNOSTIC UART ----------------------------------------------------
// The only pins brought out to pads are GPIO0 and GPIO1, the UART0 pair, and
// those are the PIO USB host. So there is no serial console on this board at
// all, and that is a deliberate trade rather than an oversight.
//
// It has to be a build-time absence, not an unused peripheral: myrtos_putc
// WAITS on the UART, so an uninitialised one would hang the kernel on its first
// line. What is left is the screen, the USB console, and the dmesg ring read
// out over the debug probe -- which is how the first boot here was watched.
#define MYRTOS_HAS_DIAG_UART    0

// --- NO VIDEO YET ----------------------------------------------------------
// The panel is 800x480 RGB565 behind an ST7262, driven by PIO -- nothing like
// DVI out of HSTX, so kernel/video.c does not apply and there is no driver for
// this one yet. MYRTOS_VIDEO=none builds without video, chargen and console.
//
//   DE 20, VSYNC 21, HSYNC 22, PCLK 23, data D0..D15 on 24..39 unbroken,
//   RST 19, EN 18, backlight 40, pixel clock 20 MHz.
//
// Written down here because the numbers were the hard part to find, and the
// unbroken run of sixteen data pins is what will let PIO shift a pixel out in
// one instruction.
#define MYRTOS_LCD_DE_PIN       20
#define MYRTOS_LCD_VSYNC_PIN    21
#define MYRTOS_LCD_HSYNC_PIN    22
#define MYRTOS_LCD_PCLK_PIN     23
#define MYRTOS_LCD_DATA0_PIN    24           // through 39, RGB565
#define MYRTOS_LCD_RST_PIN      19
#define MYRTOS_LCD_EN_PIN       18
#define MYRTOS_LCD_BL_PIN       40
#define MYRTOS_LCD_PCLK_HZ      20000000u

// --- TOUCH -----------------------------------------------------------------
// A GT911 on I2C1, five points, with an interrupt. The RTC is on the same bus.
#define MYRTOS_TOUCH_INT_PIN    16
#define MYRTOS_TOUCH_RST_PIN    17
#define MYRTOS_TOUCH_I2C_ADDR   0x5du
#define MYRTOS_RTC_I2C_ADDR     0x51u        // PCF85063

// --- USB HOST ON PIO -------------------------------------------------------
// The RX/TX pads, with a USB-A socket soldered on. GPIO0 and GPIO1 are
// adjacent with the lower first, which is PIO_USB_PINOUT_DPDM and the same
// convention the Fruit Jam uses.
//
// The 5V is wired permanently, so unlike the Fruit Jam there is no way to
// power-cycle the port from software. That matters: on the other board a
// power cycle was more than once the only thing that recovered a wedged
// device.
#define MYRTOS_HAS_PIO_USB_HOST 0
#define MYRTOS_USB_HOST_DP          0        // D- is GPIO1
#define MYRTOS_HAS_USB_HOST_POWER   0

// --- WHAT IS ABSENT --------------------------------------------------------
// No ESP32-C6, so no ESP-Hosted and no radio: lwIP here can only ever be the
// USB cable. Nothing holds a hub, a DAC and a radio in reset either, so there
// is no peripheral reset to release.
#define MYRTOS_HAS_ESP_HOSTED   0
#define MYRTOS_HAS_PERIPH_RESET 0

// --- WHAT THE HARDWARE FIXES -----------------------------------------------
// The LCD's twenty pins and the touch controller's four are the board, not a
// driver's choice, so they are claimed at boot. The USB host pair is claimed
// even before there is a driver for it, because a person asking for GPIO0 at a
// shell should be told what has it.
#define MYRTOS_BOARD_FIXED_PINS                                              \
    { 0, "usb host" }, { 1, "usb host" },                                     \
    { 6, "i2c" }, { 7, "i2c" },                                               \
    { 16, "touch" }, { 17, "touch" },                                         \
    { 18, "lcd" }, { 19, "lcd" }, { 20, "lcd" }, { 21, "lcd" },               \
    { 22, "lcd" }, { 23, "lcd" }, { 24, "lcd" }, { 25, "lcd" },               \
    { 26, "lcd" }, { 27, "lcd" }, { 28, "lcd" }, { 29, "lcd" },               \
    { 30, "lcd" }, { 31, "lcd" }, { 32, "lcd" }, { 33, "lcd" },               \
    { 34, "lcd" }, { 35, "lcd" }, { 36, "lcd" }, { 37, "lcd" },               \
    { 38, "lcd" }, { 39, "lcd" }, { 40, "lcd backlight" },                    \
    { 47, "psram" },

#endif
