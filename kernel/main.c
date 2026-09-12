#include <stdint.h>
#include <stdbool.h>
#include "tlsf.h"
#include "../common/modules.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "io.h"
#include "trap.h"
#include "sdcard.h"
#include "fat32.h"
#include "moddir.h"
#include "flashmod.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
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
// MYRTOS_UART and MYRTOS_UART_TX_PIN come from the board header.
// MYRTOS_UART_TX_PIN comes from the board header; see kernel/board.h.
#include "board.h"
#define MYRTOS_UART_BAUD   115200

// A board without a diagnostic UART has none because its only broken-out pins
// are doing something else -- see boards/ws43b.h. It has to be an absence at
// build time rather than a peripheral left alone: myrtos_putc WAITS on the
// UART, so an uninitialised one hangs the kernel on its first line.
#if MYRTOS_HAS_DIAG_UART
void myrtos_uart_init(void) {
    uart_init(MYRTOS_UART, MYRTOS_UART_BAUD);
    gpio_set_function(MYRTOS_UART_TX_PIN, UART_FUNCSEL_NUM(MYRTOS_UART, MYRTOS_UART_TX_PIN));
}
#define MYRTOS_UART_PUT(c) uart_putc_raw(MYRTOS_UART, (c))
#else
void myrtos_uart_init(void) { }
#define MYRTOS_UART_PUT(c) ((void)(c))
#endif

void myrtos_putc(char c) {
    // Every line the system prints goes through here -- the kernel's own output
    // and every module's write syscall alike -- so hooking the screen on at this
    // one point puts all of it on the display without touching a single caller.
    myrtos_console_putc(c);
    MYRTOS_UART_PUT(c);
}

// The kernel does not print from inside a trap, so interrupts are on here and a
// time slice can land in the middle of the string. The atomicity that module
// writes get for free the kernel must therefore take itself, with a critical
// section.
// Everything the kernel says, kept. It is a static buffer and not an allocation
// because the first message goes out before any pool exists -- the earliest
// lines are the ones worth having, and they are exactly the ones an allocator
// could not have held.
//
// A ring, so a long boot loses its beginning rather than its end. That is the
// wrong way round for a boot log and the right way round for a running system,
// and the running system is what has messages nobody was watching for.
// The boot log does not fit in two kilobytes: it wrapped before the shell was
// up, so `more /var/dmesg` never showed the "System: ARM 32-bit" banner that is
// printed first of all. Like MYRTOS_HEAP_SIZE this comes from CMake and follows
// MYRTOS_VIDEO, because a framebuffer build has no room to spare and a chargen
// build has 300 kB.
#ifndef DMESG_SIZE
#define DMESG_SIZE 2048
#endif
static char dmesg_buf[DMESG_SIZE];
static uint32_t dmesg_head;      // where the next byte goes
static bool dmesg_wrapped;

// A whole line at once, with interrupts off for the copy.
//
// Feeding this a byte at a time was interleavable and was interleaved: the
// first thing /var/dmesg ever showed after the filesystem server started
// mounting at boot was "Kernel is nSD: card ready ..." with "ow the idle
// process." arriving four lines later. Eleven characters in, the server got
// the processor. The console had been given a ring copy for exactly this
// reason and dmesg had been left as a loop.
static void dmesg_write(const char *p, uint32_t n) {
    uint32_t st = save_and_disable_interrupts();
    for (uint32_t i = 0; i < n; i++) {
        dmesg_buf[dmesg_head++] = p[i];
        if (dmesg_head >= DMESG_SIZE) { dmesg_head = 0; dmesg_wrapped = true; }
    }
    restore_interrupts(st);
}

// How much there is, and one byte of it. Reading is by position rather than by
// stream so that /var/dmesg can be an ordinary file: two readers do not
// interfere, and cat can be run twice.
uint32_t myrtos_dmesg_size(void) {
    return dmesg_wrapped ? DMESG_SIZE : dmesg_head;
}

int32_t myrtos_dmesg_at(uint32_t offset) {
    uint32_t n = myrtos_dmesg_size();
    if (offset >= n) return -1;
    uint32_t start = dmesg_wrapped ? dmesg_head : 0;
    return (uint8_t)dmesg_buf[(start + offset) % DMESG_SIZE];
}

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
    // The console is a different matter, and this is where the two sinks part
    // company. Its ring takes a whole run of bytes in one copy, with interrupts
    // off for the copy alone and no UART in sight, so a line can go in whole --
    // and a line is the right unit: two writers may interleave between lines,
    // which nobody minds, but not within one. The shell used to greet the user
    // in the middle of "Kernel is now the idle process."
    //
    // The UART still gets its bytes one at a time, with interrupts on.
    // The newline goes in with the line it ends, in the same call. Sent on its
    // own it left a gap exactly one write wide, and the filesystem server's
    // first message walked straight into it: every boot put "Kernel is now the
    // idle process.SD: card ready" on the screen, on one line. Which is the
    // very thing the paragraph above says must not happen -- the rule was right
    // and the implementation stopped one character short of it.
    char line[132];
    while (*s) {
        const char *run = s;
        while (*s && *s != '\n') s++;
        uint32_t n = (uint32_t)(s - run);
        bool nl = (*s == '\n');

        if (nl && n + 2 <= sizeof line) {
            for (uint32_t i = 0; i < n; i++) line[i] = run[i];
            line[n] = '\r';
            line[n + 1] = '\n';
            myrtos_console_write(line, n + 2);
        } else {
            // A line too long for the buffer goes as it always did. It can be
            // split, and one that long is a hexdump rather than a sentence.
            if (n)  myrtos_console_write(run, n);
            if (nl) myrtos_console_write("\r\n", 2);
        }

        // dmesg takes the same line in one piece. An earlier version of this
        // comment claimed a ring could not be interleaved into; the board
        // disagreed on the next boot, because the loop that filled it ran with
        // interrupts on and a line is not one write.
        if (nl && n + 1 <= sizeof line) {
            line[n] = '\n';                  // the buffer already holds the run
            dmesg_write(line, n + 1);
        } else {
            dmesg_write(run, n);
            if (nl) dmesg_write("\n", 1);
        }

        // The UART genuinely is a byte stream, sent with interrupts on, and
        // two writers can interleave in it. That is the one sink where it is
        // left alone: it is the debugging port, it is never the only copy --
        // the console and dmesg both have the line whole -- and holding
        // interrupts off for 115200-baud characters would cost more than the
        // tidiness is worth.
        for (uint32_t i = 0; i < n; i++) MYRTOS_UART_PUT(run[i]);
        if (nl) {
            MYRTOS_UART_PUT('\r');
            MYRTOS_UART_PUT('\n');
            s++;
        }
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
    // All of it. Half a megabyte used to be kept back for the one address a
    // single-instance module could be linked at; such a module is copied and
    // relocated like any other now, so there is nothing left to reserve.
    //
    // The one-instance rule stays, and is now caution rather than necessity:
    // each instance would get its own relocated copy, so the shared writable
    // data the rule exists to prevent is no longer shared. Lifting it is a
    // separate decision about what a service module means.
    //
    // The memory is real: 'memtest reserve' wrote an address-derived pattern
    // through the whole region and read every word back, both ends included,
    // before this line changed.
    uint32_t pool_bytes = psram_bytes;
    myrtos_bulk_pool = myrtos_tlsf_create((void*)MYRTOS_PSRAM_BASE, pool_bytes);
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
// Fifty-six, down from sixty-four. Most of what a process needs no longer comes
// from here: a module that is not marked real-time takes its memory block from
// PSRAM, so this holds the kernel threads' stacks, the two shells, and whatever
// a module asks for by hand. Measured with nine processes running, thirty-six
// kilobytes of it was free in one piece.
//
// The eight kilobytes went to the USB host's CDC class, which had nowhere else
// to come from -- the framebuffer is 307200 bytes of the machine and that is
// the real answer, when the display stops being one fixed mode.
//
// Eight kilobytes came back off this in Aug 2026, and the reason is worth
// keeping. The kernel is a copy_to_ram build, so code, data and this array all
// live in the same 512 KB -- and the C library's heap is only what is left over
// afterwards. Two hundred lines of new wifi code pushed .bss to end at exactly
// 0x20080000, the top of the region, leaving sbrk nothing. The link succeeded:
// the SDK asserts that the stack does not collide with the heap, and zero bytes
// of heap does not collide with anything.
//
// The machine then stopped booting, before the console and before USB, with a
// black screen and no serial port. The SDK creates its alarm pool from the C
// heap during pre-init, got NULL, and panicked into the ebreak in _exit. Days
// went into it. See the size check at the end of CMakeLists.txt, which now
// fails the build instead.
// Forty-four, down from forty-eight, when file descriptors arrived: the open
// file table, a wider path entry and the code to drive them cost about six
// kilobytes between them, and the build's heap check refused the result rather
// than letting it become another silent panic. The framebuffer is still 307200
// bytes of the machine and still the real answer.
// Forty, and the trend is the thing to notice: sixty-four, then fifty-six,
// forty-eight, forty-four, forty. Every feature that keeps a table pays for it
// here, and the framebuffer is still 307200 bytes of a 512 kB machine. When
// this next runs out, that is the number to attack rather than this one.
//
// Thirty-six, and this time the arithmetic is worth writing down because it is
// not what it looks like. The read cache and the boot script added 708 bytes:
// 496 of code, 128 of strings, 84 of table. But the link is done with
// -z max-page-size=4096, so .bss starts on a page boundary after .data -- and
// those 128 bytes of strings pushed .data past one. .bss moved up a whole page,
// and a 708-byte change cost 4180 bytes of heap, 4096 of it padding.
//
// So this knob now moves in jumps of four kilobytes whatever the feature costs,
// and the number here says less about what was added than about which side of a
// page boundary .data happened to land on. Which makes the framebuffer more
// overdue rather than less: it is the only object big enough that removing it
// would end this entirely, and there are only nine of these jumps left.
//
// Thirty-two, for USB mass storage: a 512-byte endpoint buffer, a sector buffer
// of its own, and TinyUSB's state for a second device class. This is the tenth
// time this number has come down and the framebuffer is still 307200 bytes of a
// 512 kB machine -- eighty-five per cent of the SRAM serving a screen that
// could live in the eight megabytes of PSRAM sitting idle beside it. There are
// eight of these four-kilobyte steps left. It is now the only change worth
// making here.
// Twenty-eight, down from thirty-two on 4 Sep 2026. The USB host's CDC recovery
// pushed the C heap under the floor the build checks for, and .bss sits on a
// page boundary after .data -- so this moves in four kilobyte steps whatever the
// real shortfall was.
//
// The first attempt at this took the page and left the pool too small: the two
// shells were real-time modules, so their memory came from here, and the one
// /sd/startup needs could not be allocated. The answer was not a bigger pool but
// a shell that is not real-time -- see the note beside sh in CMakeLists.txt.
// With them in PSRAM this has nearly eight kilobytes free where it had 1748.
// 28 again. It went to 27 for an afternoon to pay for wider module names, and
// the kilobyte came back when wifi left the kernel: a library module's code is
// in PSRAM, so moving one 6.9 kB driver out returned 8236 bytes of SRAM and the
// borrowing was no longer needed.
//
// 40 on 7 Sep 2026, and this is the first time this number has gone UP. The SD
// driver left the kernel the same way wifi and fat32 did -- it and the vendored
// SDIO driver together were 11.6 kB -- and the build's C heap went from 37612
// bytes to 51608. That is headroom and not memory anyone can use: this array is
// what the module pool is made of, and it is a fixed size. So twelve of the
// fourteen kilobytes are moved into it, leaving the C heap at about what it had
// while all of that code was still resident.
//
// The two that are not moved pay for the driver's own buffers, which are now
// allocated instead of declared: DMA cannot reach PSRAM, so sdlib asks the
// kernel for 1168 bytes of control blocks and a 512-byte bounce buffer at init.
//
// The size itself now comes from CMake, because it has to follow MYRTOS_VIDEO:
// a framebuffer build has some 28 kB of SRAM left over and a chargen build has
// 300, and one number cannot be right for both. See MYRTOS_HEAP_KB.
#ifndef MYRTOS_HEAP_SIZE
#define MYRTOS_HEAP_SIZE (40 * 1024)
#endif
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

    // Which machine the code is for. A module built elsewhere has a matching
    // sync word and a matching checksum and nothing else in common with this
    // kernel, so the first instruction would be the first sign -- and it would
    // arrive with nothing to connect it to its cause. A data module has no
    // instructions and is not asked.
    if ((header->type_lang >> 8) != MYRTOS_TYPE_DATA) {
        uint32_t arch = MYRTOS_ARCH_OF(header->type_lang);
        if (arch != MYRTOS_ARCH_HERE) {
            myrtos_print("  module is for machine ");
            myrtos_print_u32(arch);
            myrtos_print(", this kernel runs ");
            myrtos_print_u32(MYRTOS_ARCH_HERE);
            myrtos_print("\n");
            return false;
        }
    }

    uint32_t *raw_ptr = (uint32_t*)header;
    uint32_t checksum = 0;
    // The header is 56 bytes, hence fourteen words, and the fourteenth IS the
    // crc field. The sum therefore covers the first thirteen -- taking fourteen
    // would count the crc into its own checksum and could never match.
    for (int i = 0; i < 13; i++) {
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
#ifdef __riscv
    myrtos_print("System: RISC-V 32-bit (Hazard3)\n");
#else
    myrtos_print("System: ARM 32-bit (Cortex-M33)\n");
#endif

    // Why the machine started. Asked because an evening went into guessing it:
    // a board that reboots silently, with no panic and no trap, looks the same
    // from the log whether it browned out, glitched, was watchdogged or was
    // simply switched on -- and those want completely different answers.
    //
    // POWMAN remembers across the reset that it is reporting on. Power-on and
    // brown-out are the two that matter here; the watchdog bits would name our
    // own reboot command, and the glitch detector is its own kind of news.
    {
        uint32_t why = *(volatile uint32_t *)(0x40100000u + 0x2cu);
        myrtos_print("Reset: ");
        if (why & 0x00010000u) myrtos_print("power-on ");
        if (why & 0x00020000u) myrtos_print("BROWN-OUT ");
        if (why & 0x00040000u) myrtos_print("run-pin ");
        if (why & 0x04000000u) myrtos_print("GLITCH-DETECTED ");
        if (why & 0x00080000u) myrtos_print("debug-port ");
        if (why & 0x01800000u) myrtos_print("watchdog ");
        if (why & 0x10000000u) myrtos_print("watchdog-psm ");
        if (!(why & 0x1FCF0000u)) myrtos_print("none of the recorded causes ");
        myrtos_print("(chip_reset ");
        myrtos_print_u32(why);
        myrtos_print(")\n");
    }
    
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

    // Become the idle process before anything can trap -- before interrupts are
    // switched on, and before the self-test below makes the first system call.
    // On RISC-V this is nothing; on ARM it is what puts the kernel on the stack
    // pointer the trap vector reads, and the vector cannot work until it has
    // happened. See the trap.h for whichever machine this is.
    myrtos_arch_become_process();

    // Interrupts must be enabled globally before USB starts; individual sources
    // are enabled by whoever needs them. The timer would otherwise do it later.
    //
    // One bit in a control register on either machine, reached by its own
    // instruction: mstatus.MIE here, PRIMASK there, and cpsie is how ARM clears
    // the latter.
#ifdef __riscv
    __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));
#else
    __asm__ volatile("cpsie i" ::: "memory");
#endif

    myrtos_usb_init();

    // Prove the trap path before anything relies on it. If we get back here the
    // vector has saved, the handler has run, the saved pc has moved past the
    // call and the return has taken -- the whole chain in one call. Neither
    // half of that sentence names a machine any more, and neither does the
    // test: the cause is asked what it was rather than compared with a number.
    extern volatile uint32_t myrtos_trap_count;
    extern volatile uint32_t myrtos_last_cause;
    uint32_t before = myrtos_trap_count;
    myrtos_syscall(SYS_NULL, 0, 0, 0);
    if (myrtos_trap_count == before + 1 && MYRTOS_CAUSE_IS_SYSCALL(myrtos_last_cause)) {
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
    // Before any descriptor is added, so that a driver claiming a pin in its
    // configure finds the board's own fixed functions already there.
    myrtos_pins_init();

    // Before anything else has a chance to fill the log: if the last run ended
    // badly, that is the first thing anybody wants to read.
    { extern void myrtos_crash_report(void); myrtos_crash_report(); }
    myrtos_io_init();
    // Before any volume can be added, and before the first path is resolved.
    // /dev exists from here on, so the root is never empty.
    { extern void myrtos_vfs_init(void); myrtos_vfs_init(); }
    // /var needs no memory: the ring is already full of what has been said.
    { extern void myrtos_varfs_init(void); myrtos_varfs_init(); }
    myrtos_moddir_init();

    // Flash first: resident modules run where they lie and cost no heap. The
    // card may add to them, and a module of the same name there is registered
    // alongside -- whichever was registered first wins the lookup.
    myrtos_bulk_pool_init();
    // After the bulk pool, not with the other volumes: /tmp puts its files in
    // PSRAM, and there is no PSRAM to put them in until now.
    { extern void myrtos_tmpfs_init(void); myrtos_tmpfs_init(); }
    myrtos_pio_probe();
    // The network device's MAC, from the chip's own unique id, so that two of
    // these boards on one desk do not answer to the same address.
    {
        extern void myrtos_usb_net_id(const uint8_t *unique, uint32_t n);
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        myrtos_usb_net_id(id.id, sizeof id.id);
    }

    // Step 5 turns this on for the Waveshare board, once its PIO USB host has
    // been tried. Until then the pads are wired and unasked.
#if MYRTOS_HAS_PIO_USB_HOST
    myrtos_usbhost_init();
#endif
    // And then the host itself, on the other core. Its interrupts belong to
    // whichever core enables them, so tuh_init runs over there rather than
    // here -- see core1_main in usbhost.c.
    { extern void myrtos_usbhost_start_core1(void); myrtos_usbhost_start_core1(); }

    // After the USB host, and not by preference. Pico-PIO-USB claims DMA
    // channel zero by a hardcoded mask, so anything that claims channels
    // dynamically has to go second -- ours took channel zero first, and the
    // library's dma_claim_mask asserted on a channel already spoken for. The
    // system stopped before the USB process had started, so both consoles went
    // quiet at once and it looked like the clock change had broken everything.
    // A board with no video driver yet builds without video.c, chargen.c and
    // console.c alike -- console.c draws through one of the two and has no
    // meaning without them. MYRTOS_VIDEO=none is that build; see the Waveshare
    // board, whose panel is RGB behind an ST7262 and nothing like DVI.
#if !MYRTOS_VIDEO_NONE
    myrtos_video_init();
    myrtos_console_init();
#endif

    // Everything printed before this point went to the UART and to dmesg, and
    // to nothing else: the screen did not exist yet. That is a hundred and
    // fifty lines of boot, the "System: ARM 32-bit" banner among them, and
    // scrolling back could never reach it because it was never on the screen.
    //
    // So replay the log into the console now that there is one. It goes through
    // myrtos_console_write rather than myrtos_print, which is what keeps it out
    // of dmesg and stops the ring being copied into itself.
    {
        uint32_t n = myrtos_dmesg_size();
        char line[128];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t c = myrtos_dmesg_at(i);
            if (c < 0) break;
            line[k++] = (char)c;
            if (k == sizeof(line) || c == '\n') { myrtos_console_write(line, k); k = 0; }
        }
        if (k) myrtos_console_write(line, k);
    }
#if !MYRTOS_VIDEO_NONE
    myrtos_console_start_server();
#endif
    extern void myrtos_fs_start_server(void);
    myrtos_fs_start_server();

    // After the console and the servers, so that a chip which is not there says
    // so on a screen that exists rather than taking the boot down with it.
    // Only where there is a radio to probe. A board without one has no
    // ESP-Hosted at all and lwIP is then only ever the USB cable.
#if MYRTOS_HAS_ESP_HOSTED
    extern void myrtos_wifi_probe(void);
    myrtos_wifi_probe();
#endif
    extern void myrtos_wifi_start_server(void);
    myrtos_wifi_start_server();

    myrtos_flash_scan();

    // The card is not touched here, and that is the point. It must be asked for
    // SDIO before anything speaks SPI to it -- a card latches into SPI mode the
    // moment it is addressed that way and stays there until the power is cut --
    // so whoever brings it up has to be the first to touch it. And it cannot be
    // this function: the SDIO driver's waits are unbounded, this runs before the
    // scheduler, and a hang here takes the console and USB with it.
    //
    // The filesystem server does it instead, as the first thing its thread does
    // -- see fs_thread in fsserver.c, which calls card_bring_up and then reads
    // /sd/config.txt. It has a scheduler, so it can wait; this function has not.
    //
    // That is a change from how this worked at first, when nothing touched the
    // card until the user typed `mount`. The comment here still said so long
    // after it stopped being true, which is the sort of thing one believes at
    // three in the morning: the credentials and the hostname live on the card,
    // so waiting for a typed command would have meant a board that never joined
    // a network by itself. `mount` still exists and still works; it is no
    // longer what wakes the card.
    //
    // Modules on the card are registered when it comes up rather than here, and
    // nothing the machine needs to boot lives there anyway: the shell and every
    // descriptor are resident in flash.

    // Descriptors first: the devices must exist before any process tries to
    // open them. A data module has no entry point and is not started.
    const uint32_t nmodules = myrtos_moddir_count();   // a walk of flash; ask once
    for (uint32_t i = 0; i < nmodules; i++) {
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
            .desc = { .device_name = "term", .driver_name = "uart",
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
    const uint32_t nmods = myrtos_moddir_count();
    for (uint32_t i = 0; !started && i < nmods; i++) {
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
    while (1) {
        // Nothing. USB is serviced by its own process, not here: doing it in the
        // idle process meant anything busy at a higher priority silenced the
        // console in both directions, since received bytes reach TinyUSB's FIFO
        // only when tud_task runs.
        //
        // A tick count used to be printed here every two seconds. It was how the
        // starving DMA chain was found, and once the display had a console on it
        // the line was simply in the way.
    }
}

// Hand the board back to the bootloader. The ROM function is reached through
// rom_func_lookup, which the SDK implements for RISC-V as well as Arm, so this
// works from the Hazard3 core -- the BOOTSEL button and this end up in the same
// place.
void myrtos_reboot_bootsel(void) {
    reset_usb_boot(0, 0);
}

// Start the machine again, which is the other half of what the BOOTSEL button
// and the reset button do between them. The watchdog with a zero entry point
// means an ordinary boot: the bootrom runs, the image is copied to RAM again,
// and everything comes up as it does from power-on -- except the card, which
// does not lose power and so stays latched into whatever bus it was using.
void myrtos_reboot_machine(void) {
    watchdog_reboot(0, 0, 0);
    for (;;) { }                // it does not come back; this is for the compiler
}

// The Pico SDK's crt0 calls main once clocks and runtime are set up.
int main(void) {
    // Before anything that depends on a clock rate, the UART included. HSTX
    // shifts two bits per cycle and needs five cycles per TMDS character, so
    // the pixel clock is clk_hstx/5, and clk_hstx is pointed at clk_sys.
    //
    // PIO-USB does mind, which is what the note here used to wonder about. It
    // derives its bit clocks by dividing clk_sys down -- 48 MHz to transmit at
    // full speed, 96 MHz to receive -- and a PIO divider has eight fractional
    // bits. 125 divides into neither: transmit lands 0.05 % low and receive
    // 0.10 % high. 120 divides into both exactly, which is why every account of
    // this library says to run at 120 MHz.
    //
    // The cost is the picture: 24 MHz through the same divide by five instead
    // of 25, so 640x480 arrives at about 57 Hz rather than 60. Monitors take it.
    // The gain is that a bus which a hub has to resynchronise and repeat is at
    // last clocked at the rate it is specified for.
    set_sys_clock_khz(120000, true);

    myrtos_uart_init();
    myrtos_kernel_main();
    return 0;
}
