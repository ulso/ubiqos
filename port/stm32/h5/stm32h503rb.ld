/* The STM32H503RB's memory: 128 kB of flash from 0x08000000, two banks of 64
 * kB, and 32 kB of SRAM from 0x20000000 -- SRAM1 and SRAM2 back to back. No
 * TrustZone on this part at all.
 *
 * The same sections as stm32h563zi.ld, in less room. The code runs in flash
 * where it lies, behind the instruction cache.
 */

ENTRY(ubiqos_reset)

/* The kernel has the first bank, and the module image starts where it ends --
 * UBIQOS_BOARD_FLASH_MODULE_BASE in boards/nucleo-h503rb.h, which must say the
 * same. A kernel that outgrows it fails to link rather than running into the
 * modules. There is no fixed data area on this board. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

/* The main stack, for the reset handler, the kernel before it starts the
 * scheduler, and every exception after. Half of what the H563 gives it, and a
 * first guess: measure before cutting it further. */
__stack_size = 4K;

SECTIONS
{
    .vectors : ALIGN(512)
    {
        KEEP(*(.vectors))
    } > FLASH

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH

    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    /* Kept across a warm reset: the startup code neither loads nor clears it.
     * The kernel's crash record lives here, so that the reboot that follows a
     * crash can still say what it was. */
    .uninitialized_data (NOLOAD) : ALIGN(4)
    {
        *(.uninitialized_data .uninitialized_data.*)
    } > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    .stack (NOLOAD) : ALIGN(8)
    {
        __stack_limit = .;
        . += __stack_size;
        __stack_top = .;
    } > RAM

    /* The module pool: everything the kernel did not take. Processes, the
     * copies of modules that cannot run in place, and their heaps all come
     * from here -- see UBIQOS_POOL_FROM_LINKER in kernel/main.c. Nothing else
     * may use it: there is no C heap, and _sbrk in stubs.c refuses. */
    .pool (NOLOAD) : ALIGN(8)
    {
        __pool_start = .;
    } > RAM
    __pool_end = ORIGIN(RAM) + LENGTH(RAM);
}
