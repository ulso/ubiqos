#include <stdint.h>
#include <stdbool.h>
#include "tlsf.h"
#include "../common/modules.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "io.h"
#include "sdcard.h"
#include "fat32.h"
#include "moddir.h"
#include "flashmod.h"
#include "pico/bootrom.h"
#include "hardware/psram.h"
#include "pico/time.h"

void myrtos_pio_probe(void);
void myrtos_video_init(void);
void myrtos_video_testcard(void);
void myrtos_usbhost_init(void);
#include "usbdev.h"
#include "video.h"

// --- MYRTOS KONSTANTER ---
#define MYRTOS_SYNC_CODE 0x0509000B

// --- HARDWARE MAPPING AND PRINT HELPERS ---
// Fruit Jam, RP2350B. The SDK's PICO_DEFAULT_UART for this board is UART1 on
// GP8/GP9, but those go to the on-board ESP32-C6 rather than to any header.
// Diagnostics therefore go on UART0 TX / GP44, the same choice made in
// pico-io-fruit-jam, so the same cable works.
#define MYRTOS_UART        uart0
#define MYRTOS_UART_TX_PIN 44
#define MYRTOS_UART_BAUD   115200

void myrtos_uart_init(void) {
    uart_init(MYRTOS_UART, MYRTOS_UART_BAUD);
    gpio_set_function(MYRTOS_UART_TX_PIN, UART_FUNCSEL_NUM(MYRTOS_UART, MYRTOS_UART_TX_PIN));
}

void myrtos_putc(char c) {
    // Every line the system prints goes through here -- the kernel's own output
    // and every module's write syscall alike -- so hooking the screen on at this
    // one point puts all of it on the display without touching a single caller.
    myrtos_console_putc(c);
    uart_putc_raw(MYRTOS_UART, c);
}

// The kernel does not print from inside a trap, so interrupts are on here and a
// time slice can land in the middle of the string. The atomicity that module
// writes get for free the kernel must therefore take itself, with a critical
// section.
void myrtos_print(const char *s) {
    // This used to hold interrupts off for the whole string, so a kernel line
    // could not be interleaved with a module's. The cost turned out to be
    // unaffordable: myrtos_putc waits on the UART, and forty characters at
    // 115200 baud is three and a half milliseconds -- a hundred scanlines, and
    // the display's DMA chain dies the first time it starves.
    //
    // It ran for a third of a second and stopped, every boot, and the counter
    // that found it was the only way it was ever going to be found. Interleaved
    // diagnostics are cosmetic; a picture is not.
    while (*s) {
        if (*s == '\n') myrtos_putc('\r');
        myrtos_putc(*s++);
    }
}

void myrtos_print_u32(uint32_t v);

void myrtos_print_hex(uint32_t v) {
    const char *d = "0123456789ABCDEF";
    char buf[9];
    for (int i = 7; i >= 0; i--) { buf[i] = d[v & 15]; v >>= 4; }
    buf[8] = 0;
    myrtos_print(buf);
}

void myrtos_print_u32(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = '0' + (v % 10); v /= 10; }
    myrtos_print(&buf[i]);
}

// How much PSRAM the board turned out to have. Zero on a board without any,
// and the second pool is simply not created.
extern tlsf_pool_t myrtos_bulk_pool;
static uint32_t psram_bytes;
uint32_t myrtos_psram_bytes(void) { return psram_bytes; }

// The second pool covers the whole PSRAM window. Nothing that matters for
// timing goes here -- module code runs from SRAM and so do the stacks -- but a
// framebuffer or a file buffer has no business eating the seventy kilobytes
// that were left.
static void myrtos_bulk_pool_init(void) {
    if (!psram_is_available()) {
        myrtos_print("PSRAM: none found; bulk allocations fall back to SRAM\n");
        return;
    }
    psram_bytes = (uint32_t)psram_get_size();
    myrtos_bulk_pool = myrtos_tlsf_create((void*)MYRTOS_PSRAM_BASE, psram_bytes);
    myrtos_print("PSRAM: ");
    myrtos_print_u32(psram_bytes / 1024);
    myrtos_print(" kB at 0x");
    myrtos_print_hex(MYRTOS_PSRAM_BASE);
    myrtos_print(myrtos_bulk_pool ? ", second pool ready\n" : ", pool refused\n");
}

// --- MEMORY MANAGEMENT (TLSF) ---
// The kernel heap lives in SRAM, not PSRAM, and that is deliberate: module code
// executes from here, and PSRAM sits on QSPI behind the XIP cache with variable
// latency. The RP2350B has 512 kB of main RAM, so 320 kB for the heap leaves
// ample room for code, stacks and the kernel's own data.
//
// Once PSRAM is set up on the QMI's second chip select it belongs as a SECOND
// pool for bulk data -- not as a replacement for this one.
// The SRAM heap only holds what has timing constraints now: real-time modules
// and their processes, and the kernel's own threads. Everything else was given
// PSRAM, so 320 kB became a reservation nobody was drawing on -- and SRAM is
// what a framebuffer will want. Sixteen kilobytes are in use as this is
// written, so 64 leaves room to be wrong by a factor of four.
#define MYRTOS_HEAP_SIZE (64 * 1024)
uint8_t myrtos_heap[MYRTOS_HEAP_SIZE] __attribute__((aligned(4)));
tlsf_pool_t myrtos_mem_pool;

// Validate the header before anything in it is trusted.
bool verify_myrtos_header(myrtos_module_header_t *header) {
    if (header->sync_code != MYRTOS_SYNC_CODE) {
        return false;
    }

    // The sync word matches, so this IS a module. If the version is wrong it was
    // built against another interface, and that deserves its own message --
    // otherwise it looks like a broken checksum, which sends debugging the wrong
    // way.
    uint8_t abi = (uint8_t)(header->attr_rev & 0xff);
    if (abi != MYRTOS_ABI_VERSION) {
        myrtos_print("  module built for ABI version ");
        myrtos_print_u32(abi);
        myrtos_print(", this kernel speaks ");
        myrtos_print_u32(MYRTOS_ABI_VERSION);
        myrtos_print("\n");
        return false;
    }

    uint32_t *raw_ptr = (uint32_t*)header;
    uint32_t checksum = 0;
    // The header is 44 bytes, hence eleven words, and the eleventh IS the crc
    // field. The sum therefore covers the first ten -- taking eleven would count
    // the crc into its own checksum and could never match.
    for (int i = 0; i < 10; i++) {
        checksum += raw_ptr[i];
    }
    checksum = ~checksum;

    return (header->header_crc == checksum);
}

// --- THE SYSTEM'S ENTRY POINT ---
void myrtos_kernel_main(void) {
    myrtos_print("\n========================================\n");
    myrtos_print("      MYRTOS KERNEL v0.1 STARTING       \n");
    myrtos_print("========================================\n");
    myrtos_print("System: RISC-V 32-bit (Hazard3/Virt) mode\n");
    
    // 1. Initiera TLSF-minnespoolen
    myrtos_print("Initializing TLSF O(1) Real-Time Memory Pool...\n");
    myrtos_mem_pool = myrtos_tlsf_create(myrtos_heap, MYRTOS_HEAP_SIZE);
    
    if (myrtos_mem_pool) {
        myrtos_print("🎉 Success: Memory engine active!\n");
    } else {
        myrtos_print("❌ Error: Memory engine failed to initialize.\n");
    }

    // 2. Enable the trap vector for system calls (from scheduler.S)
    extern void myrtos_trap_vector(void);
    // mtvec is left alone: the SDK's crt0 has already pointed it at its vector
    // table, and our handlers replaced the weak entries at link time. Taking it
    // over made our timer work, but the SDK's interrupt registration stopped
    // working and TinyUSB asserted in dcd_init.
    // Prove that freed memory really is coalesced again. The blocks are freed in
    // an order that requires both forward and backward coalescing: the middle
    // first, so it has two busy neighbours, then the first and last the last.
    {
        size_t before = myrtos_tlsf_largest_free(myrtos_mem_pool);
        void *a = myrtos_tlsf_malloc(myrtos_mem_pool, 8192);
        void *b = myrtos_tlsf_malloc(myrtos_mem_pool, 8192);
        void *c = myrtos_tlsf_malloc(myrtos_mem_pool, 8192);
        myrtos_tlsf_free(myrtos_mem_pool, b);
        myrtos_tlsf_free(myrtos_mem_pool, a);
        myrtos_tlsf_free(myrtos_mem_pool, c);
        size_t after = myrtos_tlsf_largest_free(myrtos_mem_pool);

        myrtos_print("Heap coalescing: largest free block ");
        myrtos_print_u32((uint32_t)before);
        myrtos_print(" -> ");
        myrtos_print_u32((uint32_t)after);
        myrtos_print(after == before ? " (fully reclaimed)\n" : " (FRAGMENTED)\n");
    }

    myrtos_print("Trap handlers installed in the SDK vector table.\n");

    // Interrupts must be enabled globally before USB starts; individual sources
    // are enabled by whoever needs them. The timer would otherwise do it later.
    __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));

    myrtos_usb_init();

    // Prove the trap path before anything relies on it. If we get back here the
    // vector has saved, the handler has run, mepc has stepped past the ecall and
    // mret has returned -- the whole chain in one call.
    extern volatile uint32_t myrtos_trap_count;
    extern volatile uint32_t myrtos_last_mcause;
    uint32_t before = myrtos_trap_count;
    register uint32_t sys_id __asm__("a7") = 0;   // SYS_NULL
    __asm__ volatile("ecall" : : "r"(sys_id));
    if (myrtos_trap_count == before + 1 && (myrtos_last_mcause & 0x7fffffffu) == 11) {
        myrtos_print("Trap vector self-test passed: ecall taken and resumed.\n");
    } else {
        myrtos_print("Trap vector self-test FAILED.\n");
    }

    // 3. Find the module. Under QEMU, -device loader put it at 0x80500000; on
    //    hardware there is no such thing, so the module rides along in flash
    //    until it can be read from SD. The kernel only ever sees a pointer to a
    //    module header, so changing the source later touches nothing below this
    //    line.
    // --- MODULE DIRECTORY AND PROCESSES ------------------------------------
    // The modules are registered once each. A process is then created by
    // LINKING the module rather than copying it: the code is shared, only the
    // data area is private. That is OS-9's F$Link, and the reason the system
    // fitted in 64 kB.
    extern void myrtos_scheduler_init(void);
    extern int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr, const char *args);
    extern void myrtos_timer_init(uint32_t);

    myrtos_scheduler_init();
    myrtos_io_init();
    myrtos_moddir_init();

    // Flash first: resident modules run where they lie and cost no heap. The
    // card may add to them, and a module of the same name there is registered
    // alongside -- whichever was registered first wins the lookup.
    myrtos_bulk_pool_init();
    myrtos_pio_probe();
    myrtos_usbhost_init();

    // After the USB host, and not by preference. Pico-PIO-USB claims DMA
    // channel zero by a hardcoded mask, so anything that claims channels
    // dynamically has to go second -- ours took channel zero first, and the
    // library's dma_claim_mask asserted on a channel already spoken for. The
    // system stopped before the USB process had started, so both consoles went
    // quiet at once and it looked like the clock change had broken everything.
    myrtos_video_init();
    myrtos_console_init();
    myrtos_console_start_server();

    myrtos_flash_scan();

    // The buffer a module is read into on its way from the card. It is touched
    // once per module and never again, which makes it exactly the wrong thing
    // to keep 32 kB of SRAM for. PSRAM if there is any, and it is handed back
    // as soon as the card has been read.
    uint8_t *staging = myrtos_bulk_pool
        ? myrtos_tlsf_malloc(myrtos_bulk_pool, 32 * 1024)
        : myrtos_tlsf_malloc(myrtos_mem_pool, 32 * 1024);
    const uint32_t staging_size = 32 * 1024;
    if (!staging) myrtos_print("No buffer to read modules into; card ignored.\n");

    if (staging && myrtos_sd_init() && myrtos_fat_mount()) {
        char name[12];
        for (uint32_t i = 0; myrtos_fat_find_nth("MOD", i, name); i++) {
            int32_t n = myrtos_fat_read_file(name, staging, staging_size);
            if (n <= 0) continue;
            if (myrtos_moddir_add_copy(staging, (uint32_t)n, name)) {
                myrtos_print("Registered ");
                myrtos_print(name);
                myrtos_print(" from card, ");
                myrtos_print_u32((uint32_t)n);
                myrtos_print(" bytes\n");
            }
        }
    } else {
        myrtos_print("SD: unavailable.\n");
    }

    if (staging) {
        myrtos_tlsf_free(myrtos_bulk_pool ? myrtos_bulk_pool : myrtos_mem_pool, staging);
    }

    // Descriptors first: the devices must exist before any process tries to
    // open them. A data module has no entry point and is not started.
    for (uint32_t i = 0; i < myrtos_moddir_count(); i++) {
        const myrtos_module_entry_t *e = myrtos_moddir_entry(i);
        if ((e->header->type_lang >> 8) != MYRTOS_TYPE_DATA) continue;
        myrtos_print("Descriptor ");
        myrtos_print(e->name);
        myrtos_print("\n");
        myrtos_io_add_descriptor(
            (const myrtos_descriptor_t*)((const uint8_t*)e->header + sizeof(myrtos_module_header_t)));
    }

    // With no descriptors there are no devices, and then no module can print
    // anything. The kernel falls back on the device it already uses for its own
    // diagnostics, so the system never goes mute merely because the card is
    // missing a descriptor.
    if (!myrtos_io_device_count()) {
        myrtos_print("No descriptors found; registering the built-in console.\n");
        static const struct {
            myrtos_descriptor_t desc;
            myrtos_uart_config_t uart;
        } fallback = {
            .desc = { .device_name = "term", .driver_name = "UART    MOD",
                      .device_class = MYRTOS_CLASS_CHAR, .reserved = 0,
                      .config_offset = sizeof(myrtos_descriptor_t),
                      .config_size = sizeof(myrtos_uart_config_t) },
            .uart = { .uart_base = 0x40070000u, .tx_pin = 44,
                      .rx_pin = 0xffffffffu, .baud_rate = 115200 },
        };
        myrtos_io_add_descriptor(&fallback.desc);
    }

    // If there is a shell only that is started, and it starts the rest on
    // demand. Starting everything at boot was a demonstration, not a system.
    // Started before the shell, so the console is being serviced by the time
    // anything can type at it.
    myrtos_usb_start_task();

    uint32_t started = 0;
    const char *shell = myrtos_moddir_match("sh");
    if (shell) {
        const myrtos_module_header_t *m = myrtos_moddir_link(shell);
        int32_t pid = m ? myrtos_process_create(m, "") : -1;
        if (pid >= 0) {
            // Give the shell its standard paths. Everything it starts
            // inherits them, so a utility neither opens nor knows any device.
            const char *console = "usb";
            if (myrtos_io_open_as(console, pid, MYRTOS_STDIN) < 0) {
                console = "term";
                myrtos_io_open_as(console, pid, MYRTOS_STDIN);
            }
            myrtos_io_open_as(console, pid, MYRTOS_STDOUT);
            myrtos_io_open_as(console, pid, MYRTOS_STDERR);
            myrtos_print("Shell started on '");
            myrtos_print(console);
            myrtos_print("'; nothing else runs until asked.\n");
            started = 1;
        }

        // A second shell on the machine's own console, if the display and the
        // keyboard are both there. Two shells, two sets of paths, one system:
        // the serial line keeps working while the board also stands on its own
        // with nothing attached but a monitor and a keyboard.
        if (myrtos_io_has_device("con")) {
            const myrtos_module_header_t *m2 = myrtos_moddir_link(shell);
            int32_t pid2 = m2 ? myrtos_process_create(m2, "") : -1;
            if (pid2 >= 0) {
                myrtos_io_open_as("con", pid2, MYRTOS_STDIN);
                myrtos_io_open_as("con", pid2, MYRTOS_STDOUT);
                myrtos_io_open_as("con", pid2, MYRTOS_STDERR);
                myrtos_print("Shell started on 'con' as well.\n");
            }
        }
    }

    // With no shell, start the first runnable module so the system still shows
    // a sign of life. Only the first: !started ends the loop as soon as one
    // takes, and a board with no shell is being diagnosed, not used.
    for (uint32_t i = 0; !started && i < myrtos_moddir_count(); i++) {
        const myrtos_module_entry_t *e = myrtos_moddir_entry(i);
        if ((e->header->type_lang >> 8) == MYRTOS_TYPE_DATA) continue;
        myrtos_print("Starting ");
        myrtos_print(e->name);
        myrtos_print("\n");
        const myrtos_module_header_t *m = myrtos_moddir_link(e->name);
        if (m && myrtos_process_create(m, "") >= 0) started++;

    }

    if (started) {
        myrtos_print_u32(started);
        myrtos_print(" process(es) ready. Enabling pre-emption, 1 ms quantum...\n");

        myrtos_timer_init(1000);
    } else {
        myrtos_print("Nothing to run.\n");
    }
    myrtos_print("Kernel is now the idle process.\n");
    extern volatile uint32_t myrtos_ticks;
    uint32_t reported = 0;
    while (1) {
        if (myrtos_ticks - reported >= 2000) {
            reported = myrtos_ticks;
            myrtos_print("[Kernel] idle, ticks: ");
            myrtos_print_u32(myrtos_ticks);
            myrtos_print("\n");
        }
        // USB is serviced by its own process now, not here. Doing it in the
        // idle process meant that anything busy at a higher priority silenced
        // the console in both directions -- received bytes reach TinyUSB's FIFO
        // only when tud_task runs, so even input stopped.
    }
}

// Hand the board back to the bootloader. The ROM function is reached through
// rom_func_lookup, which the SDK implements for RISC-V as well as Arm, so this
// works from the Hazard3 core -- the BOOTSEL button and this end up in the same
// place.
void myrtos_reboot_bootsel(void) {
    reset_usb_boot(0, 0);
}

// The Pico SDK's crt0 calls main once clocks and runtime are set up.
int main(void) {
    // Before anything that depends on a clock rate, the UART included. HSTX
    // shifts two bits per cycle and needs five cycles per TMDS character, so
    // the pixel clock is clk_hstx/5 -- and clk_hstx has only a two-bit divider,
    // which cannot reach 125 MHz from the SDK's default 150. So the system runs
    // at 125 and the picture is right; whether PIO-USB minds is the next thing
    // to find out.
    set_sys_clock_khz(125000, true);

    myrtos_uart_init();
    myrtos_kernel_main();
    return 0;
}
