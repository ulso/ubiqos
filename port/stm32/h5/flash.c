// Writing the STM32H5's flash: what the key store needs, and so far nothing
// more -- sectors erased whole and programmed whole.
//
// Two banks of a megabyte, 8 kB sectors, and a program unit of 128 bits: four
// words written back to back, after which the controller stores them with
// their ECC. A quad-word can be programmed once after an erase and never again
// until the next erase, which the key store's way of writing -- a whole fresh
// sector every time -- was already made for.
//
// The kernel runs from the first bank and the key store lives at the end of the
// second, so the code keeps being fetched while a sector is erased: the banks
// are read and written independently. Anything in the first bank written from
// here would stall every fetch for the length of the erase, and the key store
// is placed so that it never has to be.
//
// The instruction cache sits in front of flash for data reads as well as
// fetches, so it is invalidated after a write: otherwise the key store reads
// back what was in the sector before.

#include <stdint.h>
#include <stddef.h>
#include "stm32h5xx.h"
#include "hardware/flash.h"

#define KEY1 0x45670123u
#define KEY2 0xCDEF89ABu
#define ERRORS (FLASH_SR_WRPERR_Msk | FLASH_SR_PGSERR_Msk | FLASH_SR_STRBERR_Msk \
              | FLASH_SR_INCERR_Msk | FLASH_SR_OPTCHANGEERR_Msk)

uint32_t h5_flash_errors;            // the status bits of the last failure, for a probe

static void wait_idle(void)
{
    while (FLASH->NSSR & (FLASH_SR_BSY_Msk | FLASH_SR_WBNE_Msk | FLASH_SR_DBNE_Msk)) { }
}

static void unlock(void)
{
    if (FLASH->NSCR & FLASH_CR_LOCK_Msk) {
        FLASH->NSKEYR = KEY1;
        FLASH->NSKEYR = KEY2;
    }
    FLASH->NSCCR = ERRORS | FLASH_CCR_CLR_EOP;
}

static void lock(void)
{
    FLASH->NSCR |= FLASH_CR_LOCK_Msk;
}

static void cache_forget(void)
{
    ICACHE->CR |= ICACHE_CR_CACHEINV;
    while (ICACHE->SR & ICACHE_SR_BUSYF) { }
}

static void note_errors(void)
{
    const uint32_t e = FLASH->NSSR & ERRORS;
    if (e) { h5_flash_errors = e; FLASH->NSCCR = e; }
}

void flash_range_erase(uint32_t offset, size_t count)
{
    const uint32_t bank_size = FLASH_SIZE_DEFAULT / 2u;
    wait_idle();
    unlock();
    for (uint32_t at = offset & ~(FLASH_SECTOR_SIZE - 1u); at < offset + count; at += FLASH_SECTOR_SIZE) {
        const uint32_t bank2  = at >= bank_size;
        const uint32_t sector = (at % bank_size) / FLASH_SECTOR_SIZE;
        FLASH->NSCR = FLASH_CR_SER_Msk | (sector << FLASH_CR_SNB_Pos)
                    | (bank2 ? FLASH_CR_BKSEL_Msk : 0u);
        FLASH->NSCR |= FLASH_CR_START_Msk;
        wait_idle();
        note_errors();
    }
    FLASH->NSCR = 0;
    lock();
    cache_forget();
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count)
{
    wait_idle();
    unlock();
    FLASH->NSCR = FLASH_CR_PG_Msk;
    volatile uint32_t *dst = (volatile uint32_t *)(0x08000000u + offset);
    for (size_t done = 0; done + 16u <= count; done += 16u) {
        uint32_t w[4];
        for (int i = 0; i < 16; i++) ((uint8_t *)w)[i] = data[done + i];
        for (int i = 0; i < 4; i++) dst[i] = w[i];
        __DSB();
        wait_idle();
        note_errors();
        dst += 4;
    }
    FLASH->NSCR = 0;
    lock();
    cache_forget();
}
