// See ../../README.md. The section is NOLOAD in stm32h563zi.ld and outside the
// range the startup code clears, so what is in it survives a warm reset.
#pragma once
#define __uninitialized_ram(name) __attribute__((section(".uninitialized_data"))) name
#define __not_in_flash_func(name) name
#define __time_critical_func(name) name
