// Writing the STM32F4's flash: what the key store and the settings need --
// sectors erased whole and programmed a word at a time.
//
// The sectors are not all one size: four of 16 kB, one of 64 kB, then 128 kB
// each, seven of them on the F405's megabyte. The key store and the settings
// live in the three small ones after the first, and FLASH_SECTOR_SIZE is their
// size -- see hardware/flash.h and the board header.
//
// One bank, so the code stalls while a sector is erased: a fetch from flash
// waits until the erase is over, a quarter to half a second for a 16 kB
// sector, and an interrupt with it. That is the price of writing a key or a
// setting, which is done seldom and by hand. The kernel's own code is never in
// a sector written from here; the linker script sees to that.
//
// Programmed 32 bits at a time, the width allowed from 2.7 V up. The ART
// accelerator's caches sit in front of the flash for data as well as code, so
// they are reset after a write: otherwise the key store reads back what was
// there before.

#include <stdint.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "hardware/flash.h"

#define KEY1 0x45670123u
#define KEY2 0xCDEF89ABu
#define ERRORS (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)

uint32_t stm32_flash_errors;            // the status bits of the last failure, for a probe

static void wait_idle(void)
{
    while (FLASH->SR & FLASH_SR_BSY) { }
}

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = KEY1;
        FLASH->KEYR = KEY2;
    }
    FLASH->SR = ERRORS | FLASH_SR_EOP;   // write-one-to-clear
}

static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static void cache_forget(void)
{
    const uint32_t on = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= on;
}

static void note_errors(void)
{
    const uint32_t e = FLASH->SR & ERRORS;
    if (e) { stm32_flash_errors = e; FLASH->SR = e; }
}

// The sector an offset from the start of flash falls in, and where the next
// one starts.
static uint32_t sector_of(uint32_t at, uint32_t *next)
{
    if (at < 0x10000u) { *next = (at & ~0x3FFFu) + 0x4000u; return at / 0x4000u; }
    if (at < 0x20000u) { *next = 0x20000u; return 4u; }
    *next = (at & ~0x1FFFFu) + 0x20000u;
    return 5u + (at - 0x20000u) / 0x20000u;
}

void flash_range_erase(uint32_t offset, size_t count)
{
    wait_idle();
    unlock();
    for (uint32_t at = offset; at < offset + count; ) {
        uint32_t next;
        const uint32_t sector = sector_of(at, &next);
        FLASH->CR = FLASH_CR_SER | FLASH_CR_PSIZE_1 | (sector << FLASH_CR_SNB_Pos);
        FLASH->CR |= FLASH_CR_STRT;
        wait_idle();
        note_errors();
        at = next;
    }
    FLASH->CR = 0;
    lock();
    cache_forget();
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    wait_idle();
    unlock();
    FLASH->CR = FLASH_CR_PG | FLASH_CR_PSIZE_1;
    volatile uint32_t *dst = (volatile uint32_t *)(0x08000000u + offset);
    for (size_t done = 0; done + 4u <= count; done += 4u) {
        const uint32_t w = (uint32_t)data[done] | (uint32_t)data[done + 1] << 8
                         | (uint32_t)data[done + 2] << 16 | (uint32_t)data[done + 3] << 24;
        *dst++ = w;
        __DSB();
        wait_idle();
        note_errors();
    }
    FLASH->CR = 0;
    lock();
    cache_forget();
}
