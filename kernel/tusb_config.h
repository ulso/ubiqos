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
