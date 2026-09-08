// Who owns which pin.
//
// The board has 48 of them and most are spoken for before anything asks: the
// video wants eight, the SD card six, the USB host two, the WiFi four, the
// audio four, and so on. Until now that was knowledge spread across the
// drivers and the board header, and nothing could ask the question -- which is
// fine while every pin has exactly one user written into the code, and stops
// being fine the moment something hands pins out at run time.
//
// So: one table, one owner per pin, and a driver that wants a pin says so.
// /dev/gpio refuses what is taken, and can say who has it.
//
// It is deliberately not enforcement. Nothing stops a driver writing to a pin
// it never claimed -- that would want the memory protection unit and a great
// deal more -- and the point here is that a person at a shell asking for GP44
// is told "term has it" instead of watching their console go quiet.

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"

#define MYRTOS_PINS 48u

static const char *owner[MYRTOS_PINS];

// What this board fixes in hardware, claimed at boot so that nothing has to
// discover it the hard way. Taken from the board header rather than from
// memory: DVI 12-19, the SD card 33-39, the USB host 1-2 and its power on 11,
// the WiFi 3, 22, 28, 30, 31 and 46, the buttons 0, 4 and 5.
//
// The pins a DRIVER uses are not here. Those are claimed by the driver that
// uses them, at the moment it configures itself, so that the table says what
// is true of this boot rather than what was true of the board.
static const struct { uint8_t pin; const char *who; } board_fixed[] = {
    { 0, "boot/button1" }, { 4, "button2" }, { 5, "button3" },
    { 1, "usb host" }, { 2, "usb host" }, { 11, "usb 5V" },
    { 3, "wifi" }, { 22, "wifi reset" }, { 28, "wifi" }, { 30, "wifi" },
    { 31, "wifi" }, { 46, "wifi" },
    { 12, "video" }, { 13, "video" }, { 14, "video" }, { 15, "video" },
    { 16, "video" }, { 17, "video" }, { 18, "video" }, { 19, "video" },
    { 33, "sd card" }, { 34, "sd card" }, { 35, "sd card" }, { 36, "sd card" },
    { 37, "sd card" }, { 38, "sd card" }, { 39, "sd card" },
};

void myrtos_pins_init(void)
{
    for (uint32_t i = 0; i < sizeof board_fixed / sizeof board_fixed[0]; i++)
        owner[board_fixed[i].pin] = board_fixed[i].who;
}

// The name is not copied. Callers pass a string literal or something that
// lives as long as the machine, which every driver's own name does.
int32_t myrtos_pin_claim(uint32_t pin, const char *who)
{
    if (pin >= MYRTOS_PINS || !who) return -1;
    if (owner[pin]) return -1;
    owner[pin] = who;
    return 0;
}

int32_t myrtos_pin_release(uint32_t pin)
{
    if (pin >= MYRTOS_PINS) return -1;
    owner[pin] = 0;
    return 0;
}

const char *myrtos_pin_owner(uint32_t pin)
{
    return pin < MYRTOS_PINS ? owner[pin] : "out of range";
}
