/* The STM32F405RG's memory: 1 MB of flash from 0x08000000 in sectors of 16,
 * 64 and 128 kB; 128 kB of SRAM from 0x20000000 -- SRAM1 and SRAM2 back to
 * back -- and 64 kB of core-coupled memory at 0x10000000, which only the CPU's
 * data bus reaches: no DMA, no instruction fetch.
 *
 * Flash, sector by sector:
 *
 *   0         16 kB   the vector table, and nothing else
 *   1, 2      16 kB   the key store, two copies
 *   3         16 kB   the settings kept in flash
 *   4, 5      192 kB  the kernel
 *   6 to 11   768 kB  the modules
 *
 * The table must be at the start of flash, where the chip boots, and the key
 * store wants small sectors, so the kernel steps over them: a quarter of the
 * first sector goes unused, which is cheaper than a 128 kB sector per copy of
 * the key store. UBIQOS_BOARD_FLASH_* in boards/feather-f405.h must say the
 * same.
 *
 * RAM: the kernel's data, its stack and its tables are in CCM. SRAM has the
 * network's buffers first -- lwIP's pools and heap, TinyUSB's -- which do not
 * fit beside the tables in 64 kB, and the module pool after them: processes,
 * their stacks and heaps, and any buffer a DMA might want, which CCM could
 * not give it. The top 22 kB of CCM are the fixed data area, where
 * make_flash_image.py puts sshd's writable data so that its code can stay in
 * flash -- UBIQOS_BOARD_FIXED_DATA_* in the board header, which must say the
 * same.
 */

ENTRY(ubiqos_reset)

MEMORY
{
    VECTORS (rx) : ORIGIN = 0x08000000, LENGTH = 16K
    FLASH (rx)   : ORIGIN = 0x08010000, LENGTH = 192K
    CCM (rw)     : ORIGIN = 0x10000000, LENGTH = 42K
    FIXED (rw)   : ORIGIN = 0x1000A800, LENGTH = 22K
    RAM (rwx)    : ORIGIN = 0x20000000, LENGTH = 128K
}

/* The main stack, for the reset handler, the kernel before it starts the
 * scheduler, and every exception after. As on the H563. */
__stack_size = 8K;

SECTIONS
{
    .vectors : ALIGN(512)
    {
        KEEP(*(.vectors))
    } > VECTORS

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
    } > CCM AT > FLASH
    __data_load = LOADADDR(.data);

    /* Kept across a warm reset: the startup code neither loads nor clears it.
     * The kernel's crash record lives here. */
    .uninitialized_data (NOLOAD) : ALIGN(4)
    {
        *(.uninitialized_data .uninitialized_data.*)
    } > CCM

    /* The network's zeroed data, chosen by the object it comes from: lwIP
     * and UbiqOS's lwIP glue (every path with lwip in it), and TinyUSB.
     * Cleared by the startup code like .bss. Before .bss in this script,
     * which would otherwise take these too: the first rule to match wins. */
    .bss_sram (NOLOAD) : ALIGN(4)
    {
        __sram_bss_start = .;
        *lwip*(.bss .bss.* COMMON)
        *tinyusb*(.bss .bss.* COMMON)
        . = ALIGN(8);
        __sram_bss_end = .;
    } > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > CCM

    .stack (NOLOAD) : ALIGN(8)
    {
        __stack_limit = .;
        . += __stack_size;
        __stack_top = .;
    } > CCM

    /* The module pool: the rest of SRAM -- see UBIQOS_POOL_FROM_LINKER in
     * kernel/main.c. There is no C heap, and _sbrk in stubs.c refuses. */
    __pool_start = __sram_bss_end;
    __pool_end   = ORIGIN(RAM) + LENGTH(RAM);
}
