// The STM32F4's USB device port, OTG FS, as far as TinyUSB leaves it to the
// board: its clock, its two pins, VBUS sensing, and the interrupt. The rest --
// the controller itself, the console and everything above -- is TinyUSB's
// Synopsys driver and kernel/usbdev.c, the same code the RP2350 runs.
//
// The 48 MHz the controller needs is the main PLL's Q output, which clock.c
// sets up exactly. Without HSE there is no PLL and no USB; it says so.

#include "stm32f4xx.h"
#include "port.h"
#include "tusb.h"
#include "pico/time.h"

void ubiqos_print(const char *s);

// TinyUSB's Synopsys driver counts a millisecond in core cycles when it wakes a
// host, and expects the name CMSIS's own startup files would give the clock.
uint32_t SystemCoreClock;

uint32_t tusb_time_millis_api(void) { return (uint32_t)(time_us_64() / 1000u); }

static void pin_af10(uint32_t pin)
{
    GPIOA->MODER   = (GPIOA->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));
    GPIOA->OSPEEDR |= (3u << (pin * 2u));
    volatile uint32_t *afr = &GPIOA->AFR[pin >> 3];
    const uint32_t shift = (pin & 7u) * 4u;
    *afr = (*afr & ~(0xFu << shift)) | (10u << shift);
}

void stm32_usb_hw_init(void)
{
    SystemCoreClock = stm32_sysclk_hz;
    if (!stm32_clock_from_hse) {
        ubiqos_print("USB: no crystal, so no 48 MHz; the USB port stays off\n");
        return;
    }

    // PA11 and PA12, D- and D+, on AF10.
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    (void)RCC->AHB2ENR;
    pin_af10(11);
    pin_af10(12);

    // No VBUS sensing. With it on, the controller waits for VBUS on PA9 before
    // it pulls D+ up, and PA9 is a pin like any other on the Feather. A device
    // powered from the cable is on the bus whenever it runs anyway. As
    // TinyUSB's own boards without the pin do it, before tud_init.
    USB_OTG_FS->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
    USB_OTG_FS->GCCFG &= ~(USB_OTG_GCCFG_VBUSBSEN | USB_OTG_GCCFG_VBUSASEN);

    // With the other devices, below the kernel's critical sections. TinyUSB
    // enables it itself in tud_init.
    NVIC_SetPriority(OTG_FS_IRQn, 0x80u >> (8u - __NVIC_PRIO_BITS));
}

void OTG_FS_IRQHandler(void)
{
    tud_int_handler(0);
}
