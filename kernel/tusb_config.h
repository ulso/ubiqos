#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// TinyUSB configuration for myrtos. CFG_TUSB_MCU is normally set by TinyUSB's
// own BSP, but we do not use it -- the kernel sets up USB itself.
// The RP2350 uses the same port as the RP2040.
#define CFG_TUSB_MCU            OPT_MCU_RP2040

// The SDK's CMake puts CFG_TUSB_OS=OPT_OS_PICO on the command line, and this
// deliberately disagrees: OPT_OS_PICO makes TinyUSB reach for the SDK's mutexes
// and semaphores, which block, and every tud_task and tuh_task here runs in a
// myrtos kernel thread whose blocking is the scheduler's business. OPT_OS_NONE
// leaves it polling, which is what it is being called from a thread to do.
//
// The undef is not cosmetic. Without it the compiler warns on every TinyUSB
// source, and which value wins depends on include order -- so a file that
// somehow missed this header would build its osal structures the other way and
// disagree with everything else about their size.
#undef  CFG_TUSB_OS
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
// Bit 0 is DTR, bit 1 is RTS. Written as a number on purpose: the class driver
// guards this with #if, and CDC_CONTROL_LINE_STATE_DTR is an enum constant
// rather than a macro -- so the preprocessor reads it as an undefined name,
// evaluates it to zero, and compiles the whole request away. Symbolic, readable
// and silently nothing, which cost an afternoon: the dongle echoed everything
// typed at it and answered nothing, because a CDC-ACM device reads DTR as "the
// host has opened the port" and ours never said so.
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  3

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
#define CFG_TUD_MSC             1

// A whole sector in one transfer. The SD driver reads and writes 512 bytes at a
// time and nothing smaller is useful, so this is the natural size -- and the
// host asks for many sectors per command, which TinyUSB then feeds through this
// buffer one at a time.
#define CFG_TUD_MSC_EP_BUFSIZE  512
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0

// --- ETHERNET OVER USB -----------------------------------------------------
// NCM rather than ECM or RNDIS: it is what macOS, Linux and Windows 10 onward
// all speak without a driver, and it is what the Rust bridges on this bench
// already use, so the host side is known ground.
// It follows MYRTOS_LWIP, because a network interface with no stack behind it
// is worse than none: the host enumerates it, tries to use it and gets nothing.
#ifndef MYRTOS_LWIP
#define MYRTOS_LWIP             0
#endif
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             MYRTOS_LWIP
#define CFG_TUD_NET_MTU         1514
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256

#endif
