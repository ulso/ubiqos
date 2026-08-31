#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// TinyUSB configuration for myrtos. CFG_TUSB_MCU is normally set by TinyUSB's
// own BSP, but we do not use it -- the kernel sets up USB itself.
// The RP2350 uses the same port as the RP2040.
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

// Device and host at the same time: the console on the hardware controller,
// a keyboard on two PIO state machines. Two root ports, and the reason host
// support costs as much RAM as it does.
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define CFG_TUH_RHPORT          1
#define CFG_TUH_HUB             1
#define CFG_TUH_HID             4      // a keyboard is often two or three

// One CDC-ACM device on the host side: a BLE dongle, a sensor, a modem -- the
// serial port that is not a serial port. Buffers are one bulk packet each,
// which is what the class defaults to; anything more is buffering data the
// device driver above will take within a millisecond anyway.
#define CFG_TUH_CDC             1
#define CFG_TUH_CDC_RX_BUFSIZE  64
#define CFG_TUH_CDC_TX_BUFSIZE  64

// DTR and RTS, asserted as the device is enumerated. TinyUSB leaves them low by
// default, and a CDC-ACM device reads DTR as "the host has opened the port" --
// a BleuIO dongle echoed everything typed at it and answered nothing at all
// until this was set. There is no wire and no modem; the line is a convention,
// and the convention is that the host raises it.
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  (CDC_CONTROL_LINE_STATE_DTR | \
                                           CDC_CONTROL_LINE_STATE_RTS)

// Ask for 115200 8N1 as the device is enumerated. ACM over USB does not care --
// there is no wire to run at that rate -- but a device that reports a line
// coding likes to be told one, and doing it here saves every user of the port
// from having to.
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM \
    { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }
#define CFG_TUH_DEVICE_MAX      (CFG_TUH_HUB ? 5 : 1)
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256

#endif
