#ifndef MYRTOS_ABI_H
#define MYRTOS_ABI_H

#include <stdint.h>
// bool is a keyword only from C23 onwards, and this header is public: a
// module built to an older standard must still be able to include it.
#include <stdbool.h>

// The interface between the kernel and the modules. Everything both sides must
// agree on lives here and nowhere else -- the module format, the system call
// numbers and the calling convention.
//
// The numbers used to live in the kernel AND in every module, written out by
// hand in three places. A change in the kernel then produced no build error but
// an "unknown system call" at runtime, which is exactly the kind of silent
// drift an ABI exists to prevent.
//
// Once this stops moving, this file is what a myrtos SDK consists of: module
// development needs it, not the kernel sources.

// --- FORMAT VERSION -------------------------------------------------------
// Lives in the low byte of attr_rev. The kernel rejects modules built against
// another version rather than running them and failing somewhere obscure.
// Version 2 added the three tls_ fields; version 3 added the revision. Both
// changed the header's size and what the checksum covers, so an older module in
// a newer kernel is refused rather than misread.
// 10 on 7 Sep 2026: myrtos_fsops_t gained rename. A module built against 9
// supplies a table one pointer short, and the kernel reading ops->rename off
// the end of it would follow whatever lay after -- which is exactly what the
// version is here to prevent. Old .mod files on the card are refused rather
// than trusted.
#define MYRTOS_ABI_VERSION    10

// --- MODULE HEADER --------------------------------------------------------
#define MYRTOS_SYNC_CODE      0x0509000B

// Attributes, in the high byte of attr_rev. The field already existed and held
// one bit; this is what an attributes byte is for, and OS-9 used its the same
// way, so nothing about the header's shape had to change.
#define MYRTOS_ATTR_REENTRANT 0x01
#define MYRTOS_ATTR_REALTIME  0x02   // keep this module's memory in SRAM

// This module cannot run where it lies and needs a copy of its own. It is set
// for anything with writable data, anything with relocations to apply, and
// anything with a .bss to zero -- three reasons for one answer, worked out when
// the module is built rather than three tests at load time.
//
// It is what lets a module be written as ordinary C. A static variable used to
// be refused, because one copy of the code in flash served every process and
// they would all have shared it; with a copy per process it is simply a
// variable. Position-independent code is still worth having and still costs
// nothing -- a module without this bit runs straight out of flash, as most of
// them do -- but it is no longer the price of admission.
#define MYRTOS_ATTR_PRIVATE   0x04

// Which machine the code in a module is for, in the high nibble of type_lang's
// low byte -- the language keeps the low nibble, as it did, and nothing has
// ever read more than four bits of it.
//
// Without this a module built for one architecture looks entirely valid to a
// kernel built for another: the sync word matches, the header checksum matches,
// and the first instruction is nonsense. The failure would arrive with nothing
// to connect it to its cause, which is the worst kind to leave lying about for
// a port that has not happened yet.
//
// It is derived from the object file rather than declared, so it cannot be
// stated wrongly. Zero means nobody said, and a module with code is refused for
// it; a data module has no instructions and is not asked.
#define MYRTOS_ARCH_NONE      0
#define MYRTOS_ARCH_RV32      1
#define MYRTOS_ARCH_ARM32     2   // Thumb-2, M-profile

#define MYRTOS_ARCH_OF(tl)    (((tl) >> 4) & 0x0f)
#define MYRTOS_LANG_OF(tl)    ((tl) & 0x0f)

#ifdef __riscv
#define MYRTOS_ARCH_HERE      MYRTOS_ARCH_RV32
#elif defined(__arm__) || defined(__thumb__)
#define MYRTOS_ARCH_HERE      MYRTOS_ARCH_ARM32
#else
#define MYRTOS_ARCH_HERE      MYRTOS_ARCH_NONE
#endif

// Type, in the high byte of type_lang.
#define MYRTOS_TYPE_PROGRAM   1
#define MYRTOS_TYPE_DRIVER    2
#define MYRTOS_TYPE_DATA      3   // no code, no entry point
#define MYRTOS_TYPE_LIBRARY   4   // code, but entered through a table rather
                                  // than at one point -- see exec_offset

// Everything the server asks of a volume. A volume that cannot do something
// leaves the pointer null and the request is refused -- /dev has only stat_nth,
// because a device is not a file you can write to by name.
//
// The signatures are FAT32's unchanged, so that filesystem needs no wrappers.
typedef struct {
    int32_t (*read_at)(const char *path, uint32_t offset, uint8_t *buf, uint32_t len);
    int32_t (*write_at)(const char *path, uint32_t offset, const uint8_t *buf, uint32_t len);
    bool    (*remove)(const char *path);
    // Another name for the same file, which on a real filesystem is an edit of
    // directory entries and moves no data. A volume that cannot do it leaves
    // this null and mv is refused there rather than quietly becoming cp and rm
    // -- which is a different operation with different failure modes.
    bool    (*rename)(const char *from, const char *to);
    bool    (*mkdir)(const char *path);
    bool    (*rmdir)(const char *path);
    int32_t (*stat_nth)(const char *dirpath, uint32_t index, char *name_out, uint32_t *size_out);
    // One named entry rather than the nth: attribute byte, or -1. This is what
    // lets a program ask how long a file is, which is what "a", SEEK_END and an
    // open that can refuse a missing file all wanted.
    int32_t (*stat)(const char *path, uint32_t *size_out);
    // Module scanning: find the nth file with this extension, then read it.
    bool    (*find_nth)(const char *ext_3, uint32_t index, char *name_out);
    int32_t (*read_file)(const char *name_83, uint8_t *buf, uint32_t max_len);
} myrtos_fsops_t;

// --- LIBRARY MODULES ------------------------------------------------------
// A module the kernel calls rather than runs. OS-9 had these as Sbrtn, and the
// reason is the same one: not everything that belongs in a system belongs in
// the part that must be resident from the first instruction.
//
// A program is entered once, at module_main, and owns a process. A library is
// entered many times, from whatever context the caller is in, and owns nothing.
// So exec_offset points at a table instead of at code, and the kernel checks
// the table before it trusts a single pointer in it.
//
// What this buys, and it is the reason wifi went first: a library is loaded
// where modules are loaded, which is PSRAM. Its code is not in SRAM at all, and
// the kernel image does not carry it.
#define MYRTOS_LIB_ABI 1

// The table at exec_offset. abi first so a mismatch is caught before anything
// is called; count second so a caller can ask for entry n and be told no.
typedef struct {
    uint32_t abi;       // MYRTOS_LIB_ABI
    uint32_t count;     // how many pointers follow
    void    *fn[];      // in the order the library documents
} myrtos_lib_table_t;

// And what the kernel hands back, so a library can call home.
//
// A library runs in kernel context, so it cannot reach the kernel the way a
// program does -- there is no trap to take, and ecall from inside the kernel
// would be answering its own question. It gets addresses instead, in a table
// as versioned as its own. Twelve entries is what the wifi driver needed:
// four from the kernel, seven from the SDK's hardware layer, and strlen.
//
// Version 3 added what a driver that owns PIO and DMA needs -- the SD card
// driver -- and mem_alloc, which is SRAM and is the only memory a library may
// let DMA touch.
//
// Version 4 added the one-direction SPI transfers, so a driver can move a run
// of bytes in one transaction instead of one call per byte.
#define MYRTOS_KERNEL_API_ABI 4

// Pin function numbers, which are the SDK's and are passed straight through.
// Here so that a library needs no SDK header at all -- only this one.
#define MYRTOS_GPIO_FUNC_SPI  1u
#define MYRTOS_GPIO_IN        false
#define MYRTOS_GPIO_OUT       true

typedef struct {
    uint32_t abi;                     // MYRTOS_KERNEL_API_ABI

    void   (*print)(const char *s);
    void   (*print_u32)(uint32_t v);
    int32_t (*kernel_thread)(void (*entry)(void), uint32_t stack_bytes,
                             uint32_t priority);
    void  *(*bulk_alloc)(uint32_t bytes);   // from the pool modules come from

    // The hardware, reached through the kernel's copy of the SDK rather than
    // its own. There is one SPI block and one set of pins, and two
    // initialisations of them would be one too many -- and gpio_put and
    // gpio_get are inline in the SDK's headers, so passing them here is what
    // lets a library include nothing but this file.
    void    *spi;                     // the block a driver was given
    void   (*spi_init)(void *spi, uint32_t baud);
    void   (*spi_write_read)(void *spi, const uint8_t *out, uint8_t *in, uint32_t len);
    void   (*gpio_init)(uint32_t pin);
    void   (*gpio_set_function)(uint32_t pin, uint32_t fn);
    void   (*gpio_set_dir)(uint32_t pin, bool out);
    void   (*gpio_put)(uint32_t pin, bool value);
    bool   (*gpio_get)(uint32_t pin);
    void   (*gpio_set_pulls)(uint32_t pin, bool up, bool down);
    void   (*busy_wait_us)(uint64_t us);
    uint64_t (*time_us)(void);        // free-running, never wraps in any life here

    // The block device, for a filesystem library. Four calls is the whole of
    // what fat32 wanted from the kernel besides printing.
    bool   (*sd_init)(void);
    bool   (*sd_try_sdio)(void);
    bool   (*sd_read_block)(uint32_t lba, uint8_t *buf);
    bool   (*sd_write_block)(uint32_t lba, const uint8_t *buf);

    // --- version 3 ---------------------------------------------------------

    // SRAM, and the reason it is here rather than bulk_alloc is DMA. A library
    // lives in PSRAM, which sits behind the XIP cache on the QMI bus: what DMA
    // writes there is not reliably what the CPU reads back, and a DMA control
    // block read by the DMA engine out of PSRAM cannot even be bounced. So a
    // driver that does DMA allocates its buffers here and keeps its code where
    // every other library keeps it.
    void  *(*mem_alloc)(uint32_t bytes);

    // One character. It is here so that a library can carry its own printf --
    // the SD library does, because the small printf in the kernel existed for
    // the vendored driver alone and went with it.
    void   (*putc)(char c);

    // May DMA touch this memory? The library that does DMA is not the one that
    // knows the memory map, and the question is not "is it PSRAM": it is this,
    // and the kernel is free to answer it differently later.
    bool   (*dma_safe)(const void *p);

    // The rest of the SDK a PIO driver needs. Everything here is a real
    // function rather than an inline, so a library has no other way to reach
    // it -- the PIO and DMA registers themselves are fixed addresses and need
    // nothing.
    void   (*spi_set_baudrate)(void *spi, uint32_t baud);
    void   (*sleep_ms)(uint32_t ms);
    int32_t (*pio_add_program)(void *pio, const void *program);
    void   (*pio_sm_init)(void *pio, uint32_t sm, uint32_t initial_pc,
                          const void *config);
    void   (*pio_sm_set_pindirs_with_mask64)(void *pio, uint32_t sm,
                                             uint64_t values, uint64_t mask);
    void   (*pio_set_gpio_base)(void *pio, uint32_t base);

    // The UART, for the terminal driver. uart_putc_raw is inline in the SDK's
    // header and compiles into the module; this is the only real function it
    // needs.
    void   (*uart_init)(void *uart, uint32_t baud);

    // --- version 4 ---------------------------------------------------------

    // A run of bytes in one transaction rather than one call each.
    //
    // spi_write_read above takes both buffers and was used a byte at a time,
    // which made a four-kilobyte read four thousand calls through this table,
    // each a separate blocking transfer. These are the SDK's own one-direction
    // forms: spi_read repeats one byte on the wire rather than wanting a
    // filler buffer as long as the read, which for four kilobytes matters.
    void   (*spi_write)(void *spi, const uint8_t *src, uint32_t len);
    void   (*spi_read)(void *spi, uint8_t repeated_tx, uint8_t *dst, uint32_t len);
} myrtos_kernel_api_t;

// One table for every library, which is the simple thing and not the right one
// for ever: wifi ignores the block device and fat32 ignores the SPI pins. When
// a third library wants something neither needs, this should become a common
// part and a domain part rather than growing again.

// --- DRIVER MODULES -------------------------------------------------------
// A device driver, on the card or in the module pool rather than in the kernel.
//
// The descriptors below have said which driver handles a device since they were
// written, and adding a device has been a matter of adding a file for just as
// long -- but the driver the descriptor named had to be one of the six compiled
// into io.c. This is the other half: the driver is a file too.
//
// It is a library with a fixed shape. A library publishes whatever list of
// functions it documents and the caller has to know the order; a driver
// publishes the one table the I/O manager already calls every device through,
// so there is nothing to agree about.
//
// The vtable itself lives here rather than in the kernel's io.h for the same
// reason myrtos_fsops_t does: a module cannot include a kernel header.
typedef struct {
    const char *module_name;    // what the descriptor refers to: "uart"
    int32_t (*configure)(const void *config, uint32_t size);
    int32_t (*open)(void);
    int32_t (*write)(const uint8_t *buf, uint32_t len);
    int32_t (*read)(uint8_t *buf, uint32_t len);   // 0 = nothing right now
    // Whether a read would return anything. A driver without this is never
    // waited on: reads from it keep returning 0, as they did before blocking
    // existed. That is what keeps the send-only UART from parking a shell
    // forever on input that cannot arrive.
    int32_t (*readable)(void);
    // Room to write. Absent means always writable, which is right for a driver
    // that cannot fill up -- the UART writes a byte at a time and blocks in
    // hardware, so waiting on it would never end.
    int32_t (*writable)(void);
    int32_t (*close)(void);
    // Whether a read of nothing means "never" rather than "not yet". A device
    // without this can only fall quiet, and a reader waits; /dev/null has an
    // end, and a reader that waited for it would wait for ever.
    int32_t (*at_eof)(void);
} myrtos_driver_t;

#define MYRTOS_DRIVER_ABI 1

// What exec_offset points at in a driver module, under the symbol
// myrtos_driver. init comes first and is called once, like a library's entry
// zero, because a driver has no other way to reach the kernel either.
typedef struct {
    uint32_t abi;               // MYRTOS_DRIVER_ABI
    uint32_t reserved;
    bool (*init)(const myrtos_kernel_api_t *api);
    myrtos_driver_t ops;
} myrtos_driver_module_t;

// --- DEVICE DESCRIPTORS ---------------------------------------------------
// A data module describing a device, in the OS-9 sense. It states what the
// device is called, which driver module handles it, and carries a tail that
// only that driver understands.
//
// The point is that a device can be added by dropping a file on the card
// rather than rebuilding the kernel. To change UART or baud rate you change
// the descriptor.

// A keyboard layout, carried in the descriptor's configuration tail so it can
// be changed by replacing a module on the card rather than rebuilding anything.
// Indexed by HID usage code; three levels, because a Swedish keyboard needs
// AltGr for the braces a programmer cannot do without.
//
// Characters are Latin-1, which is what the console's font draws. Dead keys are
// not dead here: the acute and the diaeresis produce themselves, since holding a
// key back until the next one needs state the driver does not yet keep.
// Eleven characters and a NUL. Module names, device names and process names are
// all this long, and the number was written out at each of them.
// A module's name, and a device's. Sixteen rather than twelve, and no longer
// 8.3: a module name may now be anything a filename may be, minus the space.
// Twelve was eight characters, three for an extension and a terminator, and
// every part of that was FAT's rather than ours -- the card could not give a
// longer name when this was written, and has been able to for days.
//
// Sixteen and not more because SRAM decided it. Thirty-two module entries and
// eight devices with two names each all carry this field, so every byte here
// costs about forty-eight in the kernel's data; twenty-four took the C heap
// below the floor check_heap.cmake enforces and the build stopped. Fifteen
// usable characters is more than any module here wants.
//
// The space is left out on purpose. It was the padding that made "sh" and
// "sh      " the same string, which is the kind of equality that has to be
// remembered everywhere it is compared; and a name with a space in it cannot be
// typed as one word at a shell prompt, so it could never have been used.
#define MYRTOS_NAME_LEN       16

#define MYRTOS_KEYMAP_KEYS    104

typedef struct {
    uint8_t plain[MYRTOS_KEYMAP_KEYS];
    uint8_t shift[MYRTOS_KEYMAP_KEYS];
    uint8_t altgr[MYRTOS_KEYMAP_KEYS];
} myrtos_keymap_t;

// Which font the console draws in, and the grid that results. The font is a
// property of the screen rather than of whoever writes to it: a process asks
// for one and every process sees the change, in the way that changing the
// keyboard layout above changes it for everyone reading the keyboard.
typedef struct {
    uint8_t index;   // which font is current
    uint8_t cell_w, cell_h;
    uint8_t count;         // how many the kernel has
    uint16_t cols, rows;   // the grid that cell gives on this display
} myrtos_confont_t;

#define MYRTOS_CLASS_CHAR  1   // character stream: terminal, serial port
#define MYRTOS_CLASS_BLOCK 2   // block oriented: SD, disk

typedef struct __attribute__((packed, aligned(4))) {
    char device_name[MYRTOS_NAME_LEN];   // what a process opens: "term"
    char driver_name[MYRTOS_NAME_LEN];   // the module handling it: "uart"
    uint16_t device_class;               // MYRTOS_CLASS_*
    uint16_t reserved;
    uint32_t config_offset;   // from the start of the descriptor to the tail
    uint32_t config_size;     // size of the tail, zero if none
} myrtos_descriptor_t;

// The tail for the UART driver. Its layout is the driver's business alone; the
// I/O manager passes it on without interpreting it.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t uart_base;   // 0x40070000 for UART0 on the RP2350
    uint32_t tx_pin;
    uint32_t rx_pin;   // 0xffffffff if send-only
    uint32_t baud_rate;
} myrtos_uart_config_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t sync_code;     // MYRTOS_SYNC_CODE
    uint32_t module_size;   // the whole module, header included
    uint32_t name_offset;   // to the name string
    uint16_t type_lang;     // type, and the machine and language below it
    uint16_t attr_rev;      // attributes and ABI version
    uint32_t exec_offset;   // to the entry point
    uint32_t mem_size;      // RAM per process: data at the bottom, stack from the top

    // Thread-local storage: the module's own variables, one set per process.
    // The linker gathers them from every source file into one block and gives
    // each a fixed offset from tp, which is what OS-9's linker did with U. The
    // kernel copies tls_init bytes of the image at tls_offset to the base of
    // the process's area, zeroes up to tls_total, and points tp at it.
    uint32_t tls_offset;   // to the initial image, zero if there is none
    uint32_t tls_init;     // bytes to copy: the .tdata part
    uint32_t tls_total;    // bytes to reserve: .tdata plus .tbss

    // The module's own revision, as OS-9 had it. The directory keeps the
    // highest of a given name and refuses the rest, which is how a system was
    // patched: a newer module in the spare EPROM socket won over the one
    // soldered down, without anything else changing.
    uint16_t revision;
    uint16_t reserved;

    // Where the absolute addresses are, and what shape each one has. Eight
    // bytes an entry: the site and a four-bit kind in one word, the target
    // offset in the next, both counted from the start of the module.
    //
    // Four kinds. A module's own code is PC-relative under -mcmodel=medany, so
    // for a module that links nothing there is only kind 0, a pointer sitting
    // in data. But newlib and libgcc arrive prebuilt in the toolchain's default
    // code model and address globals absolutely with lui, which is kinds 1 to
    // 3 -- and none of those can be repaired from the instruction alone,
    // because twelve bits of immediate do not carry a target back. Hence the
    // target in the table rather than in the code.
    //
    // The table is the tail of the module and is NOT part of the image: the
    // loader copies reloc_offset bytes and reads the table from where the
    // module lies. So a pointer table costs four bytes per entry on the card
    // and nothing at all in RAM. With no entries, reloc_offset is module_size
    // and the copy is exactly what it always was.
    uint32_t reloc_offset;   // to the table, and the length of the image
    uint32_t reloc_count;    // eight-byte entries, zero if none

    // What follows the image and is not in it. .bss and .sbss hold no bytes in
    // the file -- they are NOBITS -- so a loader that copies the image has to
    // add this much and zero it. A module with no writable data has none, which
    // is every module that is shareable.
    uint32_t bss_size;

    uint32_t header_crc;   // complement of the sum of the first thirteen words
} myrtos_module_header_t;

// What has to be copied for a module to run: the header and the code, and
// nothing after them.
//
// Not the name, and that is not a saving but a correctness matter. The file is
// header, code, name, table -- but in memory .bss begins immediately after the
// code, which is exactly where the name sits in the file. Copying the name and
// zeroing past it put a module's variables on top of its own name: vtdemo's
// counter read 0x65647476, which is "vtde". So the image ends at name_offset,
// bss_size follows it, and the name is read where the module lies rather than
// carried along. The relocation table is read there too.
static inline uint32_t myrtos_module_image_size(const myrtos_module_header_t *h)
{
    return h->name_offset;
}

// --- SYSTEM CALLS ---------------------------------------------------------
// a7 carries the number, a0-a2 the arguments, a0 comes back with the result.
#define SYS_NULL      0u    // does nothing; exists to exercise the trap path
#define SYS_IO_PUTC   1u    // a0 = character
#define SYS_EXIT      2u    // ends the process, never returns
#define SYS_OPEN      3u    // a0 = device name       -> a0 = path number
#define SYS_WRITE     4u    // a0 = path, a1 = buffer, a2 = length
#define SYS_CLOSE     5u    // a0 = path
#define SYS_MODDIR    6u    // a0 = index, a1 = &myrtos_modinfo_t -> a0 = 0, -1 = end
#define SYS_MEMINFO   7u    // a0 = 0 largest free block, 1 processes -> a0 = value
#define SYS_READ      8u    // a0 = path, a1 = buf, a2 = length -> a0 = read, 0 = nothing
#define SYS_EXEC      9u    // a0 = module name, a1 = argument string -> a0 = pid
#define SYS_ARGS      10u   // a0 = buffer, a1 = length -> a0 = characters copied
#define SYS_FSDIR     11u   // a0 = &myrtos_fs_dir_t -> a0 = attr byte, -1 = end
#define SYS_FSREAD    12u   // a0 = &myrtos_fs_io_t -> a0 = bytes read, 0 = eof
#define SYS_FSWRITE   13u   // a0 = &myrtos_fs_io_t -> a0 = bytes written
#define SYS_FSREMOVE  14u   // a0 = name -> a0 = 0 ok, -1 failed
#define SYS_FSRENAME  59u   // a0 = &myrtos_fs_rename_t -> a0 = 0 ok, -1 failed
#define SYS_WAIT      15u   // a0 = pid; returns when that process has exited
#define SYS_SLEEP     16u   // a0 = milliseconds; returns when they have passed
#define SYS_SETPRIO   17u   // a0 = new priority -> a0 = the old one
#define SYS_TICKS     18u   // -> a0 = milliseconds since the timer started
#define SYS_PSINFO    19u   // a0 = slot, a1 = &myrtos_psinfo_t -> a0 = 0, -1 empty
#define SYS_BOOTSEL   20u   // reboots into the bootloader; never returns
#define SYS_ALLOC     21u   // a0 = bytes -> a0 = pointer, 0 on failure
#define SYS_FREE      22u   // a0 = pointer -> a0 = 0, -1 if not ours
#define SYS_REALLOC   23u   // a0 = pointer, a1 = bytes -> a0 = pointer
#define SYS_DATAAREA  24u   // a0 = &size or 0 -> a0 = base of this process's area
#define SYS_ALLOCBULK 25u   // a0 = bytes -> a0 = pointer, from PSRAM if there is any
#define SYS_SEND      26u   // a0 = pid, a1 = &myrtos_msg_t -> a0 = the reply status
#define SYS_RECEIVE   27u   // a0 = &myrtos_msg_t out -> a0 = sender pid
#define SYS_REPLY     28u   // a0 = status -> a0 = 0, -1 if nobody is being served
#define SYS_PIDOF     29u   // a0 = module name -> a0 = pid, -1 if not running
#define SYS_MKDIR     30u   // a0 = path -> a0 = 0 ok, -1 failed
#define SYS_CHDIR     31u   // a0 = path -> a0 = 0 ok, -1 no such directory
#define SYS_GETCWD    32u   // a0 = buf, a1 = length -> a0 = characters copied
#define SYS_RMDIR     33u   // a0 = path -> a0 = 0 ok, -1 not empty or not there
#define SYS_RECEIVETMO 44u  // a0 = msg out, a1 = milliseconds
#define SYS_SEEK      45u   // a0 = descriptor, a1 = offset, a2 = whence
#define SYS_FSSTAT    46u   // a0 = myrtos_fs_stat_t -> a0 = attributes, -1 none
#define SYS_DUP       47u   // a0 = descriptor, a1 = new one or -1 -> a0 = new one
#define SYS_PIPE      48u   // a0 = int32_t[2] out -> a0 = 0, -1 if none can be had
#define SYS_LOADMOD   49u   // a0 = module name; reads it off the card into the
                            // directory -> a0 = 0, -1 not there or no room
#define SYS_REBOOT    50u   // starts the machine again; never returns
#define SYS_USBDISK   51u   // a0 = 1 hand the card to the host, 0 take it back
                            // -> a0 = 0, or -1 if there is nothing to hand over
#define SYS_PULSE     52u   // a0 = pid, a1 = type, a2 = value; never blocks
                            // -> a0 = 0, -1 no such process or the ring is full
#define SYS_ARM       53u   // a0 = path, a1 = pulse type (0 cancels)
                            // -> a0 = 0, -1 if there is no room to watch
#define SYS_DISARM    54u   // drops every watch this process holds
                            // -> a0 = how many there were
// SYS_OPEN takes the flags in a1. Zero is MYRTOS_O_RDONLY, which is what every
// caller written before they existed passed, so none of them changed meaning.

#define MYRTOS_SEEK_SET 0u   // from the start of the file
#define MYRTOS_SEEK_CUR 1u   // from where the descriptor is now
// From the end, which needs the file's length and so cannot be answered in the
// trap: SYS_SEEK sends it to the file server, the way open and read already go.
// Worth having rather than refusing, because fseek(f, 0, SEEK_END) followed by
// ftell is how nearly every C program asks how big a file is, and a port that
// cannot do it fails in a way that looks like a bug in the port.
#define MYRTOS_SEEK_END 2u
// SEEK_END is deliberately absent. It needs the file's length, and nothing can
// answer that yet: the filesystem lists sizes when it walks a directory and has
// no stat for one named file. Asking for it returns -1 rather than a number
// that would be wrong.
#define SYS_MOUNT     34u   // -> a0 = 0 ok, -1 no card
#define SYS_REPLYTO   35u   // a0 = pid, a1 = status -> a0 = 0, -1 not waiting on us
#define SYS_WIFIVER   36u   // a0 = buffer, a1 = length -> a0 = 0 ok, -1 no answer
#define SYS_WIFISCAN  37u   // a0 = -1 to look -> a0 = count; a0 = index -> a0 = rssi
#define SYS_CONFONT   38u   // a0 = font, -1 = current, a1 = out, a2 = 1 to only look
#define SYS_WIFIADDR  43u   // a0 = buffer, a1 = length -> a0 = 0 if there is one
#define SYS_WIFIJOIN  42u   // a0 = "ssid\0pass" -> a0 = 0 joined, else the status
#define SYS_WIFIRESET 60u   // no arguments; the chip comes back not on a network
#define SYS_WIFISOCK  58u   // a0 = op, a1 = port or socket, a2 = &{buf,len}
#define SYS_READABLE  39u   // a0 = path -> a0 = bytes waiting, 0 = none, -1 = no path
#define SYS_KILL      40u   // a0 = pid -> a0 = 0 ok, -1 no such process or refused
#define SYS_FOREGRND  41u   // a0 = path, a1 = pid or 0 -> a0 = 0 ok, -1 no path
#define SYS_USBINFO   55u   // a0 = what -> a0 = that field of the USB host state
#define SYS_RANDOM    56u   // a0 = buffer, a1 = length -> a0 = bytes filled
#define SYS_CATCHINTR 57u   // a0 = pulse type, 0 to go back to being killed

// --- MESSAGES -------------------------------------------------------------
// A rendezvous, in the manner of OSE and MINIX. The sender blocks until the
// receiver has replied, which is what makes the pointer safe: the buffer cannot
// move or be rewritten while its owner is stopped. Nothing is copied and nothing
// is allocated -- there is one address space, so a pointer means the same thing
// everywhere, and the queue of waiting senders is a list through the process
// table like the ready queues and the sleep list already are.
//
// A queue can therefore never be longer than there are processes, which is why
// there is no filter on receive: the usual argument against selective receive is
// the cost of scanning an unbounded mailbox, and this one is bounded at sixteen.
// Receive takes whatever comes and the receiver dispatches on type.
//
// Arming is not the same thing and does not replace it. OSE's selective receive
// filters at the taking-out end: signals you are not ready for stay queued, so
// a state machine can leave them until it changes state. Arming filters at the
// putting-in end -- which descriptors may speak to you at all -- and its point
// is different: it brings sources that are not messages into the same waiting
// place. What is genuinely missing here is the deferral. A receiver must take
// what arrives and put aside anything it is not ready for itself, and for
// messages there is at least myrtos_reply_to, which lets a server accept a
// second request before answering the first.
typedef struct {
    uint32_t type;    // what this is; the receiver switches on it
    uint32_t len;     // how much data points at
    void *data;       // the sender's own memory, valid until the reply
    int32_t sender;   // filled in by receive; ignored on send
} myrtos_msg_t;

#define MYRTOS_MSG_WRITE     1u   // data = characters, len = how many

// The filesystem is a service. These are sent by the kernel on a process's
// behalf when it makes a filesystem call, not by the process itself: data
// points at the request the process already built, which stays valid because
// the process is blocked in send until the answer comes back.
#define MYRTOS_MSG_FS_READ   2u
#define MYRTOS_MSG_FS_WRITE  3u
#define MYRTOS_MSG_FS_REMOVE 4u
#define MYRTOS_MSG_FS_DIR    5u
#define MYRTOS_MSG_FS_MKDIR  6u
#define MYRTOS_MSG_FS_CHDIR  7u
#define MYRTOS_MSG_FS_RMDIR  8u
#define MYRTOS_MSG_FS_MOUNT  9u
#define MYRTOS_MSG_FS_OPEN   10u   // data = path -> a descriptor
#define MYRTOS_MSG_FS_FDIO   11u   // data = myrtos_fs_fdio_t
#define MYRTOS_MSG_FS_STAT   12u   // data = myrtos_fs_stat_t
#define MYRTOS_MSG_FS_LOADMOD 13u  // data = module name, without the extension
#define MYRTOS_MSG_FS_USBDISK 14u  // data = 1 give the card away, 0 take it back
#define MYRTOS_MSG_FS_EXEC   15u   // data = myrtos_fs_exec_t -> the new pid
#define MYRTOS_MSG_FS_SEEK   16u   // data = myrtos_fs_seek_t
#define MYRTOS_MSG_FS_RENAME 17u   // data = myrtos_fs_rename_t

// How long a name a directory listing may hand back, terminator included. FAT's
// 8.3 needed twelve; VFAT's long names are read now, and ".wasm" alone does not
// fit an 8.3 extension, so an application format needed more. Sixty-four is
// what the filesystem keeps; OS-9 allowed twenty-nine and nobody found it
// short. Anything that receives a name from a listing must have this much room.
#define MYRTOS_DIRNAME_MAX 65u

// How a file is being opened. The numbers are POSIX's, so myrtos_posix.h can
// alias them rather than translate. The kernel acts on them: it refuses a file
// that is not there unless one of the creating flags is given, empties it for
// TRUNC, and puts the descriptor at the end for APPEND -- all before the caller
// sees the descriptor, which is what makes them worth having in the kernel
// rather than arranged afterwards by whoever opened it.
#define MYRTOS_O_RDONLY 0u
#define MYRTOS_O_WRONLY 1u
#define MYRTOS_O_RDWR   2u
#define MYRTOS_O_CREAT  0x40u
#define MYRTOS_O_TRUNC  0x200u
#define MYRTOS_O_APPEND 0x400u

typedef struct {
    const char *name;
    uint32_t    flags;
} myrtos_fs_open_t;

// Making a process of a module is not the small operation it looks like. A
// single-instance module has to be copied to the address it was linked for,
// and for the wasm interpreter that is a third of a megabyte out of flash and
// into PSRAM -- two chip selects on one QSPI bus. Done inside a trap it is tens
// of milliseconds with interrupts off, and on 3 Sep 2026 that killed the USB
// bus every time the interpreter was started: three missed polls end a
// transfer, and TinyUSB never asks the hub again. A tight loop in a trap is an
// interrupt blackout whether or not anybody wrote it as one.
//
// So process creation is a service, like the filesystem and the WiFi
// coprocessor, and for the same reason. The pointers here may be passed raw
// because the sender blocks until the reply: its memory cannot move or go away
// while the server is reading it.
typedef struct {
    const myrtos_module_header_t *module;
    const char                   *args;
} myrtos_fs_exec_t;

// The WiFi coprocessor is a service too, for the same reason the filesystem is:
// talking to it means waiting seconds for a scan, and waiting must not happen
// inside a trap.
#define MYRTOS_MSG_WIFI_VER  10u
#define MYRTOS_MSG_WIFI_SCAN 11u
#define MYRTOS_MSG_WIFI_JOIN 12u
#define MYRTOS_MSG_WIFI_ADDR 13u

typedef struct {
    int32_t index;   // scan: -1 to look, otherwise which entry
    char *buf;
    uint32_t len;
} myrtos_wifi_req_t;

// Sockets, which the coprocessor provides rather than this machine: the chip
// carries the TCP/IP stack, so what crosses here is a socket number and bytes,
// not packets. That is why there is no network stack in myrtos and why there is
// not going to be one -- see the note at the top of modules/wifilib.
//
// One message with an operation inside it rather than five message types. The
// five would each need a syscall number too, and what they have in common -- a
// socket, a buffer, a length -- is exactly one struct.
#define MYRTOS_MSG_WIFI_SOCK 14u
// Hold the coprocessor in reset and let it come back. It loses the network.
#define MYRTOS_MSG_WIFI_RESET 15u

#define MYRTOS_SOCK_LISTEN 0u   // arg = port      -> the listening socket, or -1
#define MYRTOS_SOCK_ACCEPT 1u   // arg = that sock -> a client socket, or -1 for nobody
#define MYRTOS_SOCK_RECV   2u   // arg = client    -> bytes read; 0 means not yet
#define MYRTOS_SOCK_SEND   3u   // arg = client    -> bytes written
#define MYRTOS_SOCK_CLOSE  4u   // arg = client    -> 0
// Diagnostics, so that what the chip thinks and what this side thinks can be
// compared instead of assumed. They are separate calls rather than one packed
// number because a packed number would have to be unpacked by every caller.
#define MYRTOS_SOCK_STATE  5u   // arg = socket    -> the chip's TCP state
#define MYRTOS_SOCK_OWNER  6u   // arg = socket    -> pid, -1 nobody, -2 reaped
#define MYRTOS_SOCK_PORT   7u   // arg = socket    -> the port it serves, or 0

// TCP's own state numbers, as nina-fw reports them.
#define MYRTOS_TCP_CLOSED      0u
#define MYRTOS_TCP_LISTEN      1u
#define MYRTOS_TCP_SYN_SENT    2u
#define MYRTOS_TCP_SYN_RCVD    3u
#define MYRTOS_TCP_ESTABLISHED 4u
#define MYRTOS_TCP_FIN_WAIT_1  5u
#define MYRTOS_TCP_FIN_WAIT_2  6u
#define MYRTOS_TCP_CLOSE_WAIT  7u
#define MYRTOS_TCP_CLOSING     8u
#define MYRTOS_TCP_LAST_ACK    9u
#define MYRTOS_TCP_TIME_WAIT  10u

typedef struct {
    uint32_t op;      // MYRTOS_SOCK_*
    uint32_t arg;     // a port for LISTEN, a socket for everything else
    uint8_t *buf;
    uint32_t len;
} myrtos_wifi_sock_t;

#define MYRTOS_MEM_LARGEST_FREE 0u
#define MYRTOS_MEM_PROCESSES    1u
#define MYRTOS_MEM_BULK_FREE    2u   // largest free block in PSRAM
#define MYRTOS_MEM_BULK_SIZE    3u   // how much PSRAM there is at all
// Assertions the kernel has stepped over, and where the last one was. A failed
// TU_ASSERT in the USB stack is an unconditional ebreak on RISC-V, which the
// trap handler steps past so the machine survives -- see the note in
// syscalls.c. The count is here because the printed line goes to the screen and
// the UART, not to the USB console, so a session on the serial port could not
// otherwise tell whether anything had happened.
#define MYRTOS_MEM_ASSERTS      4u
#define MYRTOS_MEM_ASSERT_LAST  5u

// What SYS_USBINFO will tell you about the PIO USB host. This exists because
// reading the same state with a debug probe is not free: memory access on
// Hazard3 halts the processor, and PIO-USB loses transactions while it is
// stopped, so the probe produces the failure it was there to watch. Ask the
// running machine instead.
#define MYRTOS_USB_REARMS       0u   // refused asks that had to be repeated
#define MYRTOS_USB_RECOVERIES   1u   // submitted transfers found lost
#define MYRTOS_USB_REPEATKEY    2u   // the HID usage now repeating, 0 for none
#define MYRTOS_USB_KEYSIN       3u   // bytes ever pushed into the key ring
#define MYRTOS_USB_CDCREARMS    5u   // times the dongle's read was queued again
#define MYRTOS_USB_CDCGIVEUP    6u   // 1 once the dongle stopped being asked
#define MYRTOS_USB_ROOT         4u   // init | connected<<1 | fullspeed<<2 | susp<<3 | event<<8
// addr | instance<<8 | wanted<<16 | armed<<17 | idle<<24, for eight slots
#define MYRTOS_USB_HID          0x10u
// dev<<0 | ep<<8 | has_transfer<<16 | started<<17 | stalled<<18 | failed<<24
#define MYRTOS_USB_EP           0x20u
#define MYRTOS_USB_EP_COUNT     32u

// Where the second pool lives: the XIP window after sixteen megabytes of flash
// address space, which is also where our resident module region ends.
#define MYRTOS_PSRAM_BASE       0x11000000u

// Where a single-instance module is linked and loaded.
//
// Position independence exists so that one copy of a module's code can serve
// several processes, each at whatever address it is given. A module that may
// only run once has no such problem: it can be given an address and linked at
// it. That is the whole trade -- give up being relocatable, and absolute
// addresses and writable data both simply work.
//
// The last half megabyte of an eight megabyte PSRAM, kept back from the bulk
// pool. A constant rather than something worked out at run time, because the
// module has to be LINKED at it -- which is the entire point.
#define MYRTOS_SINGLE_RESERVE   (512u * 1024u)
#define MYRTOS_SINGLE_BASE      0x11780000u

// Where such a module's CODE is linked. A .mod file is the header followed by
// the payload, and the whole file is copied to the base -- so the payload lands
// one header further on, and that is the address the linker must be told. Get
// this wrong and every absolute address is off by the size of the header, which
// shows up as a misaligned store somewhere unrelated. make_module checks it.
#define MYRTOS_SINGLE_HEADER    52u
#define MYRTOS_SINGLE_TEXT      (MYRTOS_SINGLE_BASE + MYRTOS_SINGLE_HEADER)

// The call itself. It is identical in every module, so it belongs here.
#if defined(__riscv)

static inline int32_t myrtos_syscall(uint32_t id, uint32_t a, uint32_t b, uint32_t c)
{
    register uint32_t r_id __asm__("a7") = id;
    register uint32_t r_a0 __asm__("a0") = a;
    register uint32_t r_a1 __asm__("a1") = b;
    register uint32_t r_a2 __asm__("a2") = c;

    __asm__ volatile("ecall" : "+r"(r_a0) : "r"(r_id), "r"(r_a1), "r"(r_a2) : "memory");
    return (int32_t)r_a0;
}

#elif defined(__arm__) || defined(__thumb__)

// The same call under other letters: the number in r7 and the arguments in r0
// to r2, which is where Linux's ARM EABI has always put them. The answer comes
// back in r0 because the core pops the stacked r0 the handler wrote into.
//
// r7 rather than r12 because r12 is the intra-procedure scratch register and
// belongs to the compiler and the linker's veneers. r7 is the frame pointer
// only when there is one, and modules are built at -O2 where there is not --
// a module built at -O0 would fail to compile here rather than misbehave,
// which is the right way round.
static inline int32_t myrtos_syscall(uint32_t id, uint32_t a, uint32_t b, uint32_t c)
{
    register uint32_t r_id __asm__("r7") = id;
    register uint32_t r_a0 __asm__("r0") = a;
    register uint32_t r_a1 __asm__("r1") = b;
    register uint32_t r_a2 __asm__("r2") = c;

    __asm__ volatile("svc 0" : "+r"(r_a0) : "r"(r_id), "r"(r_a1), "r"(r_a2) : "memory");
    return (int32_t)r_a0;
}

#else
#error "myrtos does not know how to make a system call on this machine"
#endif

static inline int32_t myrtos_open_flags(const char *name, uint32_t flags)
{
    return myrtos_syscall(SYS_OPEN, (uint32_t)(uintptr_t)name, flags, 0);
}

// Opening for reading, which is what this always meant.
static inline int32_t myrtos_open(const char *device)
{
    return myrtos_open_flags(device, MYRTOS_O_RDONLY);
}

// Writes take what the device can hold and report how much that was, as write
// does everywhere. The kernel blocks rather than returning zero, so this loop
// waits rather than spins.
static inline int32_t myrtos_write(int32_t path, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        int32_t n = myrtos_syscall(SYS_WRITE, (uint32_t)path, (uint32_t)(uintptr_t)(p + done), len - done);
        if (n < 0)
            return n;
        done += (uint32_t)n;
    }
    return (int32_t)done;
}

// Reads block. A process waiting for input is taken off the run queue until the
// device has something, so waiting costs nothing rather than costing every
// quantum the scheduler will give it. Zero still comes back from a device that
// cannot say whether it has anything -- the send-only UART, for instance --
// because blocking on one of those would never end.
// The standard paths, the same convention as OS-9 and Unix. The kernel sets
// them up for the first process and every child inherits them, so a utility
// neither opens nor closes anything: it reads path 0 and writes path 1.
#define MYRTOS_STDIN  0
#define MYRTOS_STDOUT 1
#define MYRTOS_STDERR 2

// Inherited output if there is any, otherwise a console of our own. The
// fallback is only needed for a module started without a parent.
static inline int32_t myrtos_console(void)
{
    if (myrtos_write(MYRTOS_STDOUT, "", 0) >= 0)
        return MYRTOS_STDOUT;
    // Under /dev, and only there. Bare device names stopped reaching the device
    // table when devices were confined to /dev, and this fallback had gone on
    // asking for them by bare name ever since -- so it could not have opened
    // anything. Nothing noticed, because it is only reached by a module started
    // without a parent, and everything here has one.
    int32_t p = myrtos_open("/dev/usb");
    if (p < 0)
        p = myrtos_open("/dev/term");
    return p;
}

// A pulse: small enough to copy, so it needs no reply and never blocks.
//
// This is the other half of the pair, and the two are for different things. A
// send blocks so the receiver may read the sender's own memory without copying
// it -- that blocking is what keeps the pointer valid, and fifteen kernel paths
// depend on it. A pulse carries no pointer, so there is nothing to keep alive:
// the type and the value are copied into the kernel and the sender walks away.
// QNX makes the same distinction and calls it the same thing.
//
// It may be refused, which a message may not: -1 means the process is not there
// or the kernel's ring is full. That is the honest consequence of not blocking.
// A receiver tells the two apart by what myrtos_receive returns -- a pid for a
// message, which must be replied to, and 0 for a pulse, which must not.
static inline int32_t myrtos_pulse(int32_t pid, uint32_t type, uint32_t value)
{
    return myrtos_syscall(SYS_PULSE, (uint32_t)pid, type, value);
}

// Ask to be told when a descriptor has something, instead of asking it over and
// over. The answer arrives as a pulse whose value is the descriptor, so a
// program watching several knows which one woke. QNX calls this ionotify and
// delivers it the same way.
//
// One shot: it fires once and disarms. A device that stays readable would
// otherwise bury its watcher in pulses, and asking again is also the moment a
// program has finished with the last one. Type 0 cancels.
//
// What it is for: waiting on more than one thing. myrtos_read blocks on one
// descriptor and myrtos_receive_tmo blocks on messages and a clock, and before
// this there was no way to wait for whichever came first. Arm the descriptor
// and then wait in receive, and a keystroke, a reply and a deadline all arrive
// at the same place.
static inline int32_t myrtos_arm(int32_t path, uint32_t type)
{
    return myrtos_syscall(SYS_ARM, (uint32_t)path, type, 0);
}

// Drop every watch at once, and every notification a watch has already sent
// that is still waiting. Returns how many of both there were. For the pattern arming
// exists for: several sources armed, and only whichever speaks first matters.
// The others stay armed otherwise, and each fires one stray pulse into a
// receive that is no longer expecting it -- one each, since a watch is one
// shot, but arriving at whatever the program is doing by then.
//
// The pending ones matter as much as the watches. Several descriptors readable
// in the same millisecond all fire in the same sweep, before the program has
// run at all, so by the time it has its first pulse the others are already sent
// and disarming the watches alone would change nothing.
//
// The eight slots are the system's, not this process's, so letting go of the
// ones you have stopped caring about is a courtesy to everything else running.
static inline int32_t myrtos_disarm_all(void)
{
    return myrtos_syscall(SYS_DISARM, 0, 0, 0);
}

// Send and block until the receiver replies. The return value is the reply's
// status, so a failed write comes back as a negative number just as it would
// from a system call.
static inline int32_t myrtos_send(int32_t pid, const myrtos_msg_t *m)
{
    return myrtos_syscall(SYS_SEND, (uint32_t)pid, (uint32_t)(uintptr_t)m, 0);
}

// Wait for a message. Returns the sender's pid; the message is copied out.
static inline int32_t myrtos_receive(myrtos_msg_t *out)
{
    return myrtos_syscall(SYS_RECEIVE, (uint32_t)(uintptr_t)out, 0, 0);
}

// A tick is a millisecond, the same unit myrtos_sleep takes.
#define MYRTOS_TIMEOUT_FOREVER 0xffffffffu
// Distinct from a sender's pid, which is never negative, and from the -2 that
// means a sender is still waiting to be answered.
#define MYRTOS_RECV_TIMEOUT    (-3)

// The same, but giving up after `ms`. MYRTOS_TIMEOUT_FOREVER is exactly
// myrtos_receive; zero polls and never blocks. Anything else returns
// MYRTOS_RECV_TIMEOUT when the time runs out with nothing to show.
//
// A server that waits forever cannot notice that the thing it serves has
// stopped answering, and cannot be told to stop either. That is the reason this
// exists -- not speed, but the ability to complain.
static inline int32_t myrtos_receive_tmo(myrtos_msg_t *out, uint32_t ms)
{
    return myrtos_syscall(SYS_RECEIVETMO, (uint32_t)(uintptr_t)out, ms, 0);
}

// Release the sender that is being served. Until this is called its buffer must
// not be touched -- that is the whole guarantee.
static inline int32_t myrtos_reply(int32_t status)
{
    return myrtos_syscall(SYS_REPLY, (uint32_t)status, 0, 0);
}

// Answer a particular sender rather than the last one received. A server that
// cannot finish a request at once -- one waiting for the network, say -- puts the
// sender's pid aside, goes on serving others, and answers this one when the
// answer exists. Without it a server has exactly one request in flight, which is
// fine for a disk and useless for a protocol stack.
//
// It refuses a pid that is not blocked waiting on THIS process, so a server
// cannot release somebody else's client.
static inline int32_t myrtos_reply_to(int32_t pid, int32_t status)
{
    return myrtos_syscall(SYS_REPLYTO, (uint32_t)pid, (uint32_t)status, 0);
}

// Find a running process by its module name. A client has to be able to name the
// service it wants without anyone having written a pid down.
static inline int32_t myrtos_pidof(const char *module_name)
{
    return myrtos_syscall(SYS_PIDOF, (uint32_t)(uintptr_t)module_name, 0, 0);
}

static inline int32_t myrtos_read(int32_t path, void *buf, uint32_t len)
{
    return myrtos_syscall(SYS_READ, (uint32_t)path, (uint32_t)(uintptr_t)buf, len);
}

// Start a module by name, with a command line. OS-9's F$Link then F$Fork.
// Hand the SD card to the host as a USB disk, or take it back. Only one side
// may have it: giving it away unmounts /sd here first, and taking it back is
// followed by an ordinary mount.
// 1 gives the card away, 0 takes it back, 2 takes it back from a host that has
// gone without ejecting. Taking it back is refused with -2 while the host still
// has the volume mounted, because doing it then hangs the host.
static inline int32_t myrtos_usbdisk(uint32_t what)
{
    return myrtos_syscall(SYS_USBDISK, what, 0, 0);
}

// Start the machine again. The counterpart of myrtos_bootsel, which hands it to
// the bootloader instead. Neither returns.
static inline void myrtos_reboot(void)
{
    myrtos_syscall(SYS_REBOOT, 0, 0, 0);
}

// Nothing is read off the card until something is run from it, so a name that
// is not in the directory is not yet an answer: it may be a file on the card
// that nobody has needed until now. Ask for it once, and try again.
//
// The retry lives here rather than in the kernel because loading is a
// filesystem call and SYS_EXEC is not. A trap runs with interrupts off, so it
// cannot read a card itself; it would have to send to the filesystem server and
// block, and a syscall that blocks in the middle has to be able to resume where
// it stopped. Two calls from out here need none of that machinery, and every
// caller gets the behaviour by calling myrtos_exec as it always did.
static inline int32_t myrtos_exec(const char *module_name, const char *args)
{
    int32_t pid = myrtos_syscall(SYS_EXEC, (uint32_t)(uintptr_t)module_name,
                                 (uint32_t)(uintptr_t)args, 0);
    if (pid != -1) return pid;               // -2 is "not re-entrant", not "missing"

    if (myrtos_syscall(SYS_LOADMOD, (uint32_t)(uintptr_t)module_name, 0, 0) != 0)
        return -1;
    return myrtos_syscall(SYS_EXEC, (uint32_t)(uintptr_t)module_name,
                          (uint32_t)(uintptr_t)args, 0);
}

// A module starts like main: argc in a0, argv in a1, and argv[0] is the
// module's own name. The kernel builds the vector in the process's memory
// before starting it.
//
//     void module_main(int argc, char **argv) { ... }
//
// A module that does not care declares module_main(void) as before.

// Fetch the raw command line. Kept for anyone who would rather parse it
// themselves than go through argv.
static inline int32_t myrtos_args(char *buf, uint32_t len)
{
    return myrtos_syscall(SYS_ARGS, (uint32_t)(uintptr_t)buf, len, 0);
}

// --- FILESYSTEM -----------------------------------------------------------
// Read-only for now: the card can be listed and read, not written. Names are
// given as a person types them ("readme.txt"); the kernel pads them into the
// 8.3 form the directory stores.

#define MYRTOS_ATTR_DIRECTORY 0x10

// List one directory. Paths are absolute and slash-separated -- "/docs/notes" --
// and an empty path or "/" is the root. Four things have to cross into the
// kernel, one more than there are argument registers, so they travel as a
// struct like reads and writes already do.
typedef struct {
    const char *path;
    uint32_t index;   // starts at zero
    char *name;       // MYRTOS_DIRNAME_MAX bytes out, NUL terminated
    uint32_t *size;   // out
} myrtos_fs_dir_t;

// "SH      MOD" -> "sh.mod". The name a directory entry should be shown and
// opened under.
//
// An entry arrives either as FAT's raw eleven characters -- no dot, the
// extension implied by its position -- or as a name that is already a name: a
// long one off the card, or a device's. They are told apart exactly rather than
// by guessing, because a FAT short entry can never contain a dot: the separator
// is not stored. Eleven characters with no dot is the one case that needs
// expanding.
//
// Here rather than in each caller, because there were two copies before this --
// ls and the wasm host's fd_readdir -- and readdir would have been a third. out
// needs MYRTOS_DIRNAME_MAX bytes.
static inline void myrtos_pretty_name(const char *raw, char *out)
{
    uint32_t len = 0;
    bool dotted = false;
    while (raw[len]) { if (raw[len] == '.') dotted = true; len++; }
    if (len != 11 || dotted) {
        uint32_t i = 0;
        for (; raw[i]; i++) out[i] = raw[i];
        out[i] = 0;
        return;
    }

    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = raw[i];
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = raw[i];
            out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[n] = 0;
}

static inline int32_t myrtos_fs_dir_at(const char *path, uint32_t index, char *name_out, uint32_t *size_out)
{
    myrtos_fs_dir_t d;
    d.path = path;
    d.index = index;
    d.name = name_out;
    d.size = size_out;
    return myrtos_syscall(SYS_FSDIR, (uint32_t)(uintptr_t)&d, 0, 0);
}

// The root, for callers that have no path to give.
static inline int32_t myrtos_fs_dir(uint32_t index, char *name_out, uint32_t *size_out)
{
    return myrtos_fs_dir_at("", index, name_out, size_out);
}

// Change the calling process's current directory. Children inherit it; a
// process changing its own does not affect the one that started it, which is
// why cd has to be built into the shell rather than be a module.
static inline int32_t myrtos_chdir(const char *path)
{
    return myrtos_syscall(SYS_CHDIR, (uint32_t)(uintptr_t)path, 0, 0);
}

static inline int32_t myrtos_getcwd(char *buf, uint32_t len)
{
    return myrtos_syscall(SYS_GETCWD, (uint32_t)(uintptr_t)buf, len, 0);
}

static inline int32_t myrtos_rmdir(const char *path)
{
    return myrtos_syscall(SYS_RMDIR, (uint32_t)(uintptr_t)path, 0, 0);
}

// Take the card again from the beginning. Mounting happens once at startup, so
// a card put in or swapped while the board is running needs this.
// Ask the ESP32-C6 what firmware it is running. A probe interface rather than a
// lasting one: when the driver can do more than one thing it becomes a device
// with a name, like the keyboard and the screen, and this goes away.
static inline int32_t myrtos_wifi_version(char *buf, uint32_t len)
{
    return myrtos_syscall(SYS_WIFIVER, (uint32_t)(uintptr_t)buf, len, 0);
}

// Look for networks, then read what was found. Neither needs a name or a
// password: a scan is what the chip hears, not what it joins.
static inline int32_t myrtos_wifi_look(void)
{
    return myrtos_syscall(SYS_WIFISCAN, (uint32_t)-1, 0, 0);
}

// Join a network. The buffer holds the name and the secret as two
// NUL-terminated strings back to back -- one allocation, one thing for the
// caller to wipe afterwards.
// The address the network handed out, if any. Associating is not the same as
// being on a network; this is the difference.
static inline int32_t myrtos_wifi_address(char *buf, uint32_t len)
{
    return myrtos_syscall(SYS_WIFIADDR, (uint32_t)(uintptr_t)buf, len, 0);
}

// Reset the coprocessor. The one recovery that does not depend on the protocol
// being in a state fit to ask anything -- and it disconnects the machine, so it
// is something a person asks for rather than something a driver does quietly.
static inline int32_t myrtos_wifi_reset(void)
{
    return myrtos_syscall(SYS_WIFIRESET, 0, 0, 0);
}

static inline int32_t myrtos_wifi_join(const char *ssid_then_pass)
{
    return myrtos_syscall(SYS_WIFIJOIN, (uint32_t)(uintptr_t)ssid_then_pass, 0, 0);
}

static inline int32_t myrtos_wifi_network(int32_t index, char *ssid, uint32_t len)
{
    return myrtos_syscall(SYS_WIFISCAN, (uint32_t)index, (uint32_t)(uintptr_t)ssid, len);
}

// The buffer and its length travel together because a syscall has three
// arguments and this wants four. Everything below is one line over it.
typedef struct { uint8_t *buf; uint32_t len; } myrtos_sockbuf_t;

// The family is myrtos_sock_*, all five of it: myrtos_send and myrtos_receive
// are the message passing and were here first, and a socket send that was
// called myrtos_send would be two different things one letter apart.
//
// Listen on a port. The answer is a socket to hand to the calls below, or -1 --
// which on a machine that is not on a network is what always comes back.
static inline int32_t myrtos_sock_listen(uint16_t port)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_LISTEN, port, 0);
}

// Somebody's socket, or -1 for nobody yet. It does not wait: a server that
// wants to wait sleeps between asks, and one that has other work does it.
static inline int32_t myrtos_sock_accept(int32_t server_sock)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_ACCEPT, (uint32_t)server_sock, 0);
}

// Zero means nothing has arrived yet, not that the client has gone.
static inline int32_t myrtos_sock_recv(int32_t sock, uint8_t *buf, uint32_t len)
{
    myrtos_sockbuf_t b = { buf, len };
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_RECV, (uint32_t)sock,
                          (uint32_t)(uintptr_t)&b);
}

static inline int32_t myrtos_sock_send(int32_t sock, const uint8_t *buf, uint32_t len)
{
    myrtos_sockbuf_t b = { (uint8_t *)buf, len };
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_SEND, (uint32_t)sock,
                          (uint32_t)(uintptr_t)&b);
}

static inline int32_t myrtos_sock_close(int32_t sock)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_CLOSE, (uint32_t)sock, 0);
}

// What the chip believes, what this side believes, and which port. Asked one at
// a time by the sockstat command; nothing else needs them.
static inline int32_t myrtos_sock_state(int32_t sock)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_STATE, (uint32_t)sock, 0);
}

static inline int32_t myrtos_sock_owner(int32_t sock)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_OWNER, (uint32_t)sock, 0);
}

static inline int32_t myrtos_sock_port(int32_t sock)
{
    return myrtos_syscall(SYS_WIFISOCK, MYRTOS_SOCK_PORT, (uint32_t)sock, 0);
}

static inline const char *myrtos_tcp_state_name(uint32_t s)
{
    static const char *const names[11] = {
        "closed", "listen", "syn-sent", "syn-rcvd", "established",
        "fin-wait-1", "fin-wait-2", "close-wait", "closing", "last-ack",
        "time-wait" };
    return s < 11 ? names[s] : "?";
}

// Which bus to ask the card for. The order is the card's rule and not ours: it
// latches into SPI the moment it is addressed that way and stays there until
// the power is cut. So SDIO has to be the first thing asked after power-up, or
// it cannot be had at all -- which is why nothing touches the card at startup
// any more, and why this is a choice the user makes rather than one we guess.
#define MYRTOS_MOUNT_SPI  0u
#define MYRTOS_MOUNT_SDIO 1u

// Ask which bus the card came up on without touching it. Asking used to mean
// mounting, and since a bare mount defaulted to SPI, the question itself pulled
// a four-bit card down to one bit -- a state that then costs a power cycle to
// undo. A query has to be a query.
#define MYRTOS_MOUNT_QUERY 2u

// Returns 0 on success. -2 says the host has the card over USB. For
// MYRTOS_MOUNT_QUERY the answer is the bus itself, MYRTOS_MOUNT_SPI or
// MYRTOS_MOUNT_SDIO, and -1 means no card is mounted.
static inline int32_t myrtos_mount(uint32_t bus)
{
    return myrtos_syscall(SYS_MOUNT, bus, 0, 0);
}

// Set the console font. The grid reported back is the one that font gives.
static inline int32_t myrtos_console_font(int32_t index, myrtos_confont_t *out)
{
    return myrtos_syscall(SYS_CONFONT, (uint32_t)index, (uint32_t)(uintptr_t)out, 0);
}

// The same question without the answer taking effect. Pass -1 for whichever font
// is current, or an index to find out what that one would give.
static inline int32_t myrtos_console_font_info(int32_t index, myrtos_confont_t *out)
{
    return myrtos_syscall(SYS_CONFONT, (uint32_t)index, (uint32_t)(uintptr_t)out, 1);
}

// Is there anything to read? myrtos_read blocks when there is not -- the caller
// is put on WAIT_READ and its ecall re-executed when a byte turns up -- which is
// what you want in a loop that has nothing else to do, and exactly what you do
// not want in one that is waiting for an answer that may never come. Ask first
// and a program can give up.
static inline int32_t myrtos_readable(int32_t path)
{
    return myrtos_syscall(SYS_READABLE, (uint32_t)path, 0, 0);
}

// End another process. Refused for the kernel's own service threads, which the
// machine needs and nobody chose to start.
//
// A process blocked on a server does not go away at once: the server is holding
// a pointer into its memory, so it stops running immediately and is taken apart
// when the reply comes. It shows as "zomb" in ps until then.
// Ask a process to end. One that called myrtos_catch_intr is told and given
// half a second to go on its own -- the same treatment Ctrl-C gives, and the
// only way a background process can stop cleanly, since it is nobody's
// foreground and the key cannot reach it. Everything else ends at once.
static inline int32_t myrtos_kill(int32_t pid)
{
    return myrtos_syscall(SYS_KILL, (uint32_t)pid, 0, 0);
}

// End it now, asking nothing. What -9 has always meant.
static inline int32_t myrtos_kill_now(int32_t pid)
{
    return myrtos_syscall(SYS_KILL, (uint32_t)pid, 1, 0);
}

// Say which process the interrupt key on this path's terminal should end, and
// pass 0 when it has finished. A shell does this around a command it waits for.
//
// It has to be said rather than worked out. Ctrl-C is caught where the byte
// arrives -- the process it is meant for is usually blocked and reading nothing
// -- and at that moment the kernel has no way of telling which of several
// processes the person typing had in mind. The shell knows: it started it.
static inline int32_t myrtos_foreground(int32_t path, int32_t pid)
{
    return myrtos_syscall(SYS_FOREGRND, (uint32_t)path, (uint32_t)pid, 0);
}

static inline int32_t myrtos_mkdir(const char *path)
{
    return myrtos_syscall(SYS_MKDIR, (uint32_t)(uintptr_t)path, 0, 0);
}

// Four arguments do not fit in a0-a2, so the request travels as a struct. Reads
// and writes take the same one -- they differ in direction, not in shape -- and
// it leaves room to grow without disturbing the calling convention.
typedef struct {
    const char *name;
    uint32_t offset;
    uint8_t *buf;
    uint32_t len;
} myrtos_fs_io_t;

// Reading or writing through a descriptor, where the position is the
// descriptor's and not the caller's. The struct is filled in by the kernel, not
// by the module: myrtos_read and myrtos_write take a descriptor and know
// nothing of messages, and it is the system call that discovers the descriptor
// is a file and turns the request into one.
typedef struct {
    int32_t  fd;
    uint8_t *buf;
    uint32_t len;
    uint32_t write;                 // non-zero to write
} myrtos_fs_fdio_t;

// Seeking from the end, which is the one seek the trap cannot answer: it needs
// the file's length. The descriptor rather than the name, because the server
// keeps the path the descriptor was opened with, and a process that has since
// done cd must not find its open files moving under it.
typedef struct {
    int32_t fd;
    int32_t offset;
} myrtos_fs_seek_t;

// Asking about one named file. The size comes back through the pointer because
// the reply carries the attribute byte, and a directory is worth telling from a
// file even when both exist.
typedef struct {
    const char *name;
    uint32_t   *size;
} myrtos_fs_stat_t;

// Read a slice of a file. Returns bytes read, 0 at end of file, -1 if missing.
static inline int32_t myrtos_fs_read(const char *name, uint32_t offset, void *buf, uint32_t len)
{
    myrtos_fs_io_t r = {name, offset, (uint8_t *)buf, len};
    return myrtos_syscall(SYS_FSREAD, (uint32_t)(uintptr_t)&r, 0, 0);
}

// Write a slice, creating and extending the file as needed. Returns bytes
// written, or -1. There is no truncate: writing over a longer file leaves the
// tail behind, so a utility that replaces a file removes it first.
static inline int32_t myrtos_fs_write(const char *name, uint32_t offset, const void *buf, uint32_t len)
{
    myrtos_fs_io_t r = {name, offset, (uint8_t *)(uintptr_t)buf, len};
    return myrtos_syscall(SYS_FSWRITE, (uint32_t)(uintptr_t)&r, 0, 0);
}

// Delete a file. Returns 0, or -1 if it is missing or is a directory.
// The attribute byte, or -1 when there is no such entry. MYRTOS_ATTR_DIRECTORY
// is the bit worth testing. Size may be null if only existence matters.
static inline int32_t myrtos_fs_stat(const char *name, uint32_t *size_out)
{
    myrtos_fs_stat_t r = { name, size_out };
    return myrtos_syscall(SYS_FSSTAT, (uint32_t)(uintptr_t)&r, 0, 0);
}

// Both names, because a syscall's arguments are registers and the server needs
// them at the same time. Sent by pointer, which is safe because the sender is
// blocked in send while the server reads it.
typedef struct {
    const char *from;
    const char *to;
} myrtos_fs_rename_t;

// Rename, and on one volume that is also move. Refused across volumes: the
// filesystems do not share a cluster chain, so it would have to be a copy --
// and a copy that calls itself a move is how a full card loses a file.
static inline int32_t myrtos_fs_rename(const char *from, const char *to)
{
    myrtos_fs_rename_t r = { from, to };
    return myrtos_syscall(SYS_FSRENAME, (uint32_t)(uintptr_t)&r, 0, 0);
}

static inline int32_t myrtos_fs_remove(const char *name)
{
    return myrtos_syscall(SYS_FSREMOVE, (uint32_t)(uintptr_t)name, 0, 0);
}

// Wait for a process to exit. Returns at once if it already has, so there is no
// race between starting something and waiting for it.
static inline int32_t myrtos_wait(int32_t pid)
{
    return myrtos_syscall(SYS_WAIT, (uint32_t)pid, 0, 0);
}

// Sleep for a length of time. The tick is a millisecond, so that is the unit.
// Zero yields: the process stays runnable but lets the next one go first.
static inline int32_t myrtos_sleep(uint32_t ms)
{
    return myrtos_syscall(SYS_SLEEP, ms, 0, 0);
}

// Thirty-two levels. 0 belongs to the idle process and cannot be taken; 16 is
// what a process starts with. Strict priority: nothing below the highest ready
// level runs at all, so a process that neither blocks nor sleeps starves
// everything under it for as long as it holds the processor.
#define MYRTOS_PRIO_MAX     31
#define MYRTOS_PRIO_DEFAULT 16

// Set this process's priority, returning the previous one. Zero asks without
// changing anything: it is the idle process's level and cannot be taken, so it
// is free to mean something else.
static inline int32_t myrtos_setprio(uint32_t prio)
{
    return myrtos_syscall(SYS_SETPRIO, prio, 0, 0);
}

static inline int32_t myrtos_getprio(void)
{
    return myrtos_syscall(SYS_SETPRIO, 0, 0, 0);
}

// Milliseconds since the timer started. Wraps after 49 days; compare
// differences rather than absolute values and the wrap takes care of itself.
static inline uint32_t myrtos_ticks_now(void)
{
    return (uint32_t)myrtos_syscall(SYS_TICKS, 0, 0, 0);
}

// What a process is doing. The states a reader cares about are the ones it can
// be stuck in, so they are named rather than numbered in any output.
#define MYRTOS_PS_FREE       0
#define MYRTOS_PS_READY      1
#define MYRTOS_PS_RUNNING    2
#define MYRTOS_PS_WAIT_READ  3
#define MYRTOS_PS_WAIT_WRITE 6
#define MYRTOS_PS_WAIT_RECV  7
#define MYRTOS_PS_WAIT_REPLY 8
#define MYRTOS_PS_WAIT_CHILD 4
#define MYRTOS_PS_SLEEPING   5
#define MYRTOS_PS_ZOMBIE     9   // killed, waiting for a server to let go

typedef struct {
    uint32_t pid;
    uint32_t state;   // MYRTOS_PS_*
    uint32_t priority;
    uint32_t mem_size;            // data and stack together, as the header asked
    char name[MYRTOS_NAME_LEN];   // the module's, or a kernel thread's stand-in
} myrtos_psinfo_t;

// How many processes can exist, kernel included. This lived in three places --
// the scheduler's table, the I/O manager's path table, and here -- with nothing
// keeping them equal. Raising only the scheduler's would have given high pids no
// I/O at all: path_of would refuse every path number they asked for, and every
// read and write would fail without saying why.
//
// The cost is 88 bytes of kernel table per process, so the limit is set by what
// is useful rather than by what fits.
// Sixteen, down from thirty-two, to pay for pulses -- 2816 bytes of process
// table and 1536 of descriptor slots, which is the whole cost of the feature
// and then some. The machine runs eight processes in ordinary use: the kernel,
// four of its threads, two shells and whatever was typed. A pipeline with a
// couple of background jobs is the case that could reach the ceiling.
//
// This is what running out of SRAM looks like from the inside. The framebuffer
// is 307200 bytes of a 512 kB machine and cannot move -- writing a glyph to
// PSRAM stalls the display's own reads and the monitor drops sync, which was
// measured and is written down in video.c. So every feature from here is paid
// for out of a table like this one.
#define MYRTOS_MAX_PROCESSES 16

// How much memory each process running this module is given: the command line,
// argv, the thread-local block and the data area at the bottom, the stack from
// the top. Say nothing and it is four kilobytes, which is what every module had
// before this existed and is ample for a utility.
//
// Write it once at file scope:
//
//     MYRTOS_MEM_SIZE(8192);
//
// It becomes an ABSOLUTE symbol -- no data, no relocation, nothing in the image
// at all -- and make_module.py reads its value with nm. That is why it can be
// stated beside the code that needs it rather than in the build files, and why
// it costs the module nothing to say.
//
// The build refuses anything that is not a multiple of four between 1024 and
// 65536: below that is not a process once a 128-byte trap frame is on the
// stack, and above it cannot be satisfied out of the SRAM pool.
#define MYRTOS_MEM_SIZE(n) \
    __asm__(".globl __myrtos_mem_size\n.set __myrtos_mem_size, " #n "\n")

// Ask about one slot. Slots are not compacted, so walk from 0 to the limit and
// skip the ones that answer -1 rather than stopping at the first.
#define MYRTOS_PS_SLOTS      MYRTOS_MAX_PROCESSES

static inline int32_t myrtos_psinfo(uint32_t slot, myrtos_psinfo_t *out)
{
    return myrtos_syscall(SYS_PSINFO, slot, (uint32_t)(uintptr_t)out, 0);
}

// Reboot into the bootloader, so new firmware can be loaded without reaching
// for the board. Does not return.
static inline void myrtos_bootsel(void)
{
    myrtos_syscall(SYS_BOOTSEL, 0, 0, 0);
}

// --- MEMORY ---------------------------------------------------------------
// Memory beyond the block the module header asked for. The kernel remembers
// which process each block belongs to, so nothing is lost when a process dies
// -- including one that dies without tidying up.
//
// Where the pointer lives matters. A module may not have writable statics, so
// `static void *buf;` is refused by the build. Keep it on the stack, or in the
// data area the module header reserved.
static inline void *myrtos_alloc(uint32_t size)
{
    return (void *)(uintptr_t)myrtos_syscall(SYS_ALLOC, size, 0, 0);
}

// Large and patient: from PSRAM when the board has it, so a framebuffer or a
// file buffer does not eat the SRAM that module code and stacks run from.
// Falls back to ordinary memory rather than failing.
static inline void *myrtos_alloc_bulk(uint32_t size)
{
    return (void *)(uintptr_t)myrtos_syscall(SYS_ALLOCBULK, size, 0, 0);
}

// Returns 0, or -1 for a pointer this process was not given.
static inline int32_t myrtos_free(void *ptr)
{
    return myrtos_syscall(SYS_FREE, (uint32_t)(uintptr_t)ptr, 0, 0);
}

// NULL leaves the old block untouched, so the caller has not lost it.
static inline void *myrtos_realloc(void *ptr, uint32_t size)
{
    return (void *)(uintptr_t)myrtos_syscall(SYS_REALLOC, (uint32_t)(uintptr_t)ptr, size, 0);
}

// This process's own data area, inside the block the module header asked for.
// It is what a module uses instead of a static variable: the code is one shared
// copy, so a static would be shared too, but this is per process.
//
// It is reached rather than passed, which is the point. Every source file in a
// module can call this and get the same state without threading a pointer
// through every function -- the same relation a pimpl has to `this`, where the
// process is the object.
//
// The stack grows down into the same span, so size_out says what exists, not
// what is safe to use. A module that wants a lot should ask for a larger
// mem_size rather than assume.
//
// This is the raw area, which begins after the thread-local block. For ordinary
// variables reach for `static __thread` instead: the linker gives those fixed
// offsets from tp, so they cost one instruction and no call at all. This is for
// bytes you want to lay out yourself.
static inline void *myrtos_data_area(uint32_t *size_out)
{
    return (void *)(uintptr_t)myrtos_syscall(SYS_DATAAREA, (uint32_t)(uintptr_t)size_out, 0, 0);
}

// Move a file descriptor's position, and answer where it ended up. Only files
// have one: a device is a stream and seeking it means nothing, so it fails.
//
// myrtos_seek(fd, 0, MYRTOS_SEEK_CUR) is ftell, and costs nothing.
static inline int32_t myrtos_seek(int32_t fd, int32_t offset, uint32_t whence)
{
    return myrtos_syscall(SYS_SEEK, (uint32_t)fd, (uint32_t)offset, whence);
}

static inline int32_t myrtos_tell(int32_t fd)
{
    return myrtos_seek(fd, 0, MYRTOS_SEEK_CUR);
}

// A second descriptor onto the same thing. -1 for the lowest free number.
// This is what redirection is made of: put the file on 1, start the child, put
// the old descriptor back.
// A buffer with two ends. fds[0] reads what is written to fds[1]. The reader
// blocks while it is empty and a writer still holds the other end; once the
// last writer has closed, an empty pipe reads as end of file instead.
static inline int32_t myrtos_pipe(int32_t fds[2])
{
    return myrtos_syscall(SYS_PIPE, (uint32_t)(uintptr_t)fds, 0, 0);
}

static inline int32_t myrtos_dup(int32_t path, int32_t new_path)
{
    return myrtos_syscall(SYS_DUP, (uint32_t)path, (uint32_t)new_path, 0);
}

static inline int32_t myrtos_close(int32_t path)
{
    return myrtos_syscall(SYS_CLOSE, (uint32_t)path, 0, 0);
}

static inline void myrtos_exit(void)
{
    myrtos_syscall(SYS_EXIT, 0, 0, 0);
}

// Convenience: write a NUL-terminated string in ONE call. Sending the whole
// string rather than a character at a time is what makes the output atomic
// against other processes.
// --- FUNCTION TABLES IN RELOCATABLE CODE ----------------------------------
// An ordinary table of function pointers carries absolute addresses written in
// by the linker, and breaks position independence. Store the DISTANCE from the
// table to the function instead, and add the table's address at runtime, and
// the table becomes relocatable -- and can sit const in .rodata, hence shared
// between processes. A table built on the stack at runtime works too, but then
// every process gets its own copy.
//
// The difference has to be computed by the assembler: C rejects it as an
// initialiser, because the difference between two addresses is not a constant
// in the language's sense. The result is ADD32/SUB32 relocations, which are
// link-time constants carrying no absolute address.
//
// The functions must carry __attribute__((used)): they are referenced only
// from assembly, which the compiler cannot see, and are otherwise optimised
// away as unused.
//
//     __attribute__((used)) static void do_read(int a) { ... }
//     MYRTOS_RELTAB_BEGIN(ops);
//     MYRTOS_RELTAB_ENTRY(ops, do_read);
//     MYRTOS_RELTAB_ENTRY(ops, do_write);
//     MYRTOS_RELTAB_END();
//     ...
//     MYRTOS_RELTAB_CALL(ops, i, void (*)(int))(arg);

#define MYRTOS_RELTAB_BEGIN(name)                                                                                      \
    extern const intptr_t name[];                                                                                      \
    __asm__(".pushsection .rodata." #name ",\"a\"\n"                                                                   \
            ".balign 4\n.globl " #name "\n" #name ":")

#define MYRTOS_RELTAB_ENTRY(name, fn)         __asm__(".word " #fn " - " #name)

// .popsection is not optional: without it the section switch stays in effect
// and all following code lands in the table's section instead of .text.
#define MYRTOS_RELTAB_END()                   __asm__(".popsection")

#define MYRTOS_RELTAB_CALL(name, index, type) ((type)((intptr_t)(name) + (name)[index]))

// A write is atomic, but a LINE only is if it goes out in one call. If a
// utility builds its line from several writes, other processes get in between,
// and the output is unreadable as soon as more than one process speaks. Hence
// this: gather the line, send it once.
typedef struct {
    char buf[96];
    uint32_t len;
} myrtos_line_t;

static inline void myrtos_line_reset(myrtos_line_t *l)
{
    l->len = 0;
}

static inline void myrtos_line_str(myrtos_line_t *l, const char *s)
{
    while (*s && l->len < sizeof(l->buf) - 1)
        l->buf[l->len++] = *s++;
}

static inline void myrtos_line_chars(myrtos_line_t *l, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n && l->len < sizeof(l->buf) - 1; i++)
        l->buf[l->len++] = s[i];
}

static inline void myrtos_line_u32(myrtos_line_t *l, uint32_t v)
{
    char tmp[11];
    int i = 10;
    tmp[i] = 0;
    if (!v)
        tmp[--i] = '0';
    while (v) {
        tmp[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    myrtos_line_str(l, &tmp[i]);
}

// An address is not a quantity, and printing one in decimal costs whoever reads
// it a conversion before they can look it up. The kernel's assertion line was
// decimal the first time it ever fired in front of a user, and that is exactly
// what happened.
static inline void myrtos_line_hex(myrtos_line_t *l, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[11];
    tmp[0] = '0';
    tmp[1] = 'x';
    for (int i = 0; i < 8; i++)
        tmp[2 + i] = digits[(v >> (28 - i * 4)) & 0xfu];
    tmp[10] = 0;
    myrtos_line_str(l, tmp);
}

// One byte, two digits. myrtos_line_hex always prints eight, which is right for
// an address and wrong for anything smaller: usbstat read out its endpoint
// addresses as 0x00000081, where the number everyone quotes -- the notes, the
// USB specification, TinyUSB's own logs -- is 0x81.
static inline void myrtos_line_hex_byte(myrtos_line_t *l, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[5];
    tmp[0] = '0';
    tmp[1] = 'x';
    tmp[2] = digits[(v >> 4) & 0xfu];
    tmp[3] = digits[v & 0xfu];
    tmp[4] = 0;
    myrtos_line_str(l, tmp);
}

// Utilities need to print numbers, and a module has no printf. Ten lines here
// saves them in every utility.
static inline int32_t myrtos_write_u32(int32_t path, uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (!v)
        buf[--i] = '0';
    while (v) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    return myrtos_write(path, &buf[i], (uint32_t)(10 - i));
}

// One entry of the module directory. The revision is what decides which copy of
// a name the system keeps, so it belongs in any listing of them.
typedef struct {
    char name[MYRTOS_NAME_LEN];
    uint32_t links;      // processes running it right now
    uint32_t revision;   // the highest of this name won
    uint32_t size;       // the whole module, header included
    uint32_t type;       // MYRTOS_TYPE_*, out of the header's own byte
} myrtos_modinfo_t;

// Three letters for a listing. Not the name's extension, which says MOD on
// every module ever made and so says nothing at all.
// An array of characters, not of pointers. A switch returning string literals
// compiles to a table of addresses and check_module.py refuses the module for
// it -- which it did to lsmod the first time this was written, exactly as the
// documentation says it would.
static inline const char *myrtos_type_name(uint32_t type)
{
    static const char names[5][4] = { "???", "PRG", "DRV", "DAT", "LIB" };
    return names[type < 5 ? type : 0];
}

static inline int32_t myrtos_moddir_get(uint32_t index, myrtos_modinfo_t *out)
{
    return myrtos_syscall(SYS_MODDIR, index, (uint32_t)(uintptr_t)out, 0);
}

static inline int32_t myrtos_meminfo(uint32_t what)
{
    return myrtos_syscall(SYS_MEMINFO, what, 0, 0);
}

// Random bytes, from the ring oscillator's own random bit with the microsecond
// timer mixed in. Genuine hardware entropy, and enough to seed a hash or pick an
// identifier -- not enough to make a key out of, and it does not claim to be.
//
// It fills at most 256 bytes per call and says how many it managed, so the
// caller loops. That is not a limitation of the source but of where the work
// happens: a system call runs in a trap with interrupts off, and an unbounded
// loop in a trap is the mistake this system has made more than once.
// Hear about Ctrl-C instead of being ended by it.
//
// The key then arrives as a pulse of this type, from pid 0, in whatever receive
// the process already uses -- so a program written in the arming style needs no
// new wait for it. What it must do is end itself: half a second later the kernel
// ends it anyway, and a second press ends it at once. That is deliberate. An
// interrupt key that a program can refuse is not an interrupt key.
//
// Meant for a process that has something to undo. A scanner tells its dongle to
// stop scanning; the dongle is otherwise left running into a machine that is no
// longer listening.
static inline int32_t myrtos_catch_intr(uint32_t pulse_type)
{
    return myrtos_syscall(SYS_CATCHINTR, pulse_type, 0, 0);
}

static inline int32_t myrtos_random(void *buf, uint32_t len)
{
    return myrtos_syscall(SYS_RANDOM, (uint32_t)(uintptr_t)buf, len, 0);
}

static inline uint32_t myrtos_usbinfo(uint32_t what)
{
    return (uint32_t)myrtos_syscall(SYS_USBINFO, what, 0, 0);
}

static inline int32_t myrtos_line_flush(int32_t path, myrtos_line_t *l)
{
    int32_t r = myrtos_write(path, l->buf, l->len);
    l->len = 0;
    return r;
}

// --- COLOUR ---------------------------------------------------------------
// Changing colour is writing the bytes for it. The console understands the
// usual SGR codes, so everything written after one comes out in the new colour
// until it is changed again or reset -- there is nothing to open and no call to
// make:
//
//     myrtos_write_str(MYRTOS_STDOUT, "\x1b[33;44m");   // yellow on blue
//
// The catch is the one the line buffer above exists for. A write is atomic and
// a pair of them is not, so a colour set in one write and the text printed in
// the next will colour whatever another process printed in between. Build both
// into one myrtos_line_t and the question does not arise.
#define MYRTOS_BLACK   0u
#define MYRTOS_RED     1u
#define MYRTOS_GREEN   2u
#define MYRTOS_YELLOW  3u
#define MYRTOS_BLUE    4u
#define MYRTOS_MAGENTA 5u
#define MYRTOS_CYAN    6u
#define MYRTOS_WHITE   7u
#define MYRTOS_BRIGHT  8u    // add to any of the eight
#define MYRTOS_KEEP    16u   // leave that half of it as it is

static inline void myrtos_line_colour(myrtos_line_t *l, uint32_t fg, uint32_t bg)
{
    myrtos_line_str(l, "\x1b[");
    if (fg < 16)
        myrtos_line_u32(l, ((fg & 8u) ? 90u : 30u) + (fg & 7u));
    if (bg < 16) {
        if (fg < 16)
            myrtos_line_str(l, ";");
        myrtos_line_u32(l, ((bg & 8u) ? 100u : 40u) + (bg & 7u));
    }
    if (fg >= 16 && bg >= 16)
        myrtos_line_u32(l, 0);   // neither named: reset
    myrtos_line_str(l, "m");
}

// Back to the colours the console started in.
static inline void myrtos_line_plain(myrtos_line_t *l)
{
    myrtos_line_str(l, "\x1b[0m");
}

// When a whole write is one colour and nothing else is in flight. Two writes,
// so it is the wrong tool for one line of coloured text -- use the two above.
static inline int32_t myrtos_colour(int32_t path, uint32_t fg, uint32_t bg)
{
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_colour(&l, fg, bg);
    return myrtos_line_flush(path, &l);
}

static inline int32_t myrtos_write_str(int32_t path, const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return myrtos_write(path, s, n);
}

// --help, and -h, for a utility that takes arguments.
//
// True when the caller asked, having already printed the text, so a module opens
// with one line:
//
//     if (myrtos_help(argc, argv, "usage: rm FILE...\n")) return;
//
// Every argument is looked at rather than only the first: "rm -f --help" is a
// question, and a utility that acted on -f before noticing would be surprising.
//
// The text belongs beside the code it describes. Several of these utilities
// already had a usage line and printed it only when something went wrong, which
// is the one moment a person is least able to read it.
static inline bool myrtos_help(int argc, char **argv, const char *usage)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool ask = (a[0] == '-' && a[1] == 'h' && !a[2]);
        if (!ask && a[0] == '-' && a[1] == '-') {
            const char *w = "help";
            const char *q = a + 2;
            while (*w && *q == *w) { w++; q++; }
            ask = !*w && !*q;
        }
        if (ask) { myrtos_write_str(MYRTOS_STDOUT, usage); return true; }
    }
    return false;
}


#endif
