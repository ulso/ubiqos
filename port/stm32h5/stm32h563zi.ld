/* The STM32H563ZI's memory, as seen with TrustZone off (TZEN = 0xC3, the state
 * a NUCLEO-H563ZI leaves the factory in): 2 MB of flash from 0x08000000 and
 * 640 kB of SRAM from 0x20000000 -- SRAM1, SRAM2 and SRAM3 back to back.
 *
 * The code runs where it is, in flash. The RP2350 build copies itself into
 * SRAM because of the QMI, which shares one bus between flash and PSRAM; this
 * chip has its own flash controller and an instruction cache in front of it,
 * and nothing it runs from an interrupt can stall against anything else.
 */

ENTRY(ubiqos_reset)

/* The top 64 kB of SRAM are not the kernel's: they are the fixed data area,
 * where make_flash_image.py puts the writable data of the single-instance
 * programs whose code stays in flash -- UBIQOS_FIXED_DATA_BASE in
 * kernel/flashmod.h, which must say the same. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 576K
    FIXED (rw)  : ORIGIN = 0x20090000, LENGTH = 64K
}

/* The main stack, for the reset handler, the kernel before it starts the
 * scheduler, and every exception after. */
__stack_size = 8K;

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

    __heap_start = .;
    __heap_end = ORIGIN(RAM) + LENGTH(RAM);
}
