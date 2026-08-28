#include <stdint.h>
#include <stdbool.h>
#include "tlsf.h"
#include "../common/modules.h"  // Inkludera din befintliga fil direkt!
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "io.h"
#include "sdcard.h"
#include "fat32.h"
#include "moddir.h"
#include "flashmod.h"
#include "usbdev.h"

// --- MYRTOS KONSTANTER ---
#define MYRTOS_SYNC_CODE 0x0509000B

// --- HÅRDVARU-MAPPNING OCH UTSKRIFTSFUNKTIONER ---
// Fruit Jam, RP2350B. SDK:ns PICO_DEFAULT_UART för det här kortet är UART1 på
// GP8/GP9, men de går till ESP32-C6:an ombord och inte till någon stiftlist.
// Diagnostik läggs därför på UART0 TX / GP44, samma val som gjorts i
// pico-io-fruit-jam, så samma sladd fungerar.
#define MYRTOS_UART        uart0
#define MYRTOS_UART_TX_PIN 44
#define MYRTOS_UART_BAUD   115200

void myrtos_uart_init(void) {
    uart_init(MYRTOS_UART, MYRTOS_UART_BAUD);
    gpio_set_function(MYRTOS_UART_TX_PIN, UART_FUNCSEL_NUM(MYRTOS_UART, MYRTOS_UART_TX_PIN));
}

void myrtos_putc(char c) {
    uart_putc_raw(MYRTOS_UART, c);
}

// Kärnan skriver inte inifrån en trap, så här är avbrotten påslagna och en
// tidsdelning kan slå till mitt i strängen. Samma odelbarhet som modulernas
// skrivningar får gratis måste kärnan alltså ta själv, med en kritisk sektion.
void myrtos_print(const char *s) {
    uint32_t mstatus;
    __asm__ volatile("csrrc %0, mstatus, %1" : "=r"(mstatus) : "r"(1u << 3));
    while (*s) {
        if (*s == '\n') myrtos_putc('\r');
        myrtos_putc(*s++);
    }
    if (mstatus & (1u << 3)) {
        __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));
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

// --- MINNESHANTERING (TLSF) ---
// Kärnans heap ligger i SRAM, inte i PSRAM, och det är avsiktligt: modulkod
// exekverar härifrån, och PSRAM sitter på QSPI bakom XIP-cachen med variabel
// latens. RP2350B har 512 kB huvud-RAM, så 320 kB åt heapen lämnar gott om
// utrymme åt kod, stackar och kärnans egna data.
//
// När PSRAM väl är uppsatt på QMI:s andra chip select hör den hemma som en
// ANDRA pool för bulkdata -- inte som ersättning för den här.
#define MYRTOS_HEAP_SIZE (320 * 1024)
uint8_t myrtos_heap[MYRTOS_HEAP_SIZE] __attribute__((aligned(4)));
tlsf_pool_t myrtos_mem_pool;

// Uppdatera valideringen så den stegar igenom dina 32 bytes (8 st 32-bitars ord)
bool verify_myrtos_header(myrtos_module_header_t *header) {
    if (header->sync_code != MYRTOS_SYNC_CODE) {
        return false;
    }

    // Synkordet stämmer, så det ÄR en modul. Är versionen fel är den byggd mot
    // ett annat gränssnitt, och det förtjänar ett eget besked -- annars ser det
    // ut som en trasig checksumma, vilket leder felsökningen åt fel håll.
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
    // Huvudet är 28 byte, alltså sju ord, och det sjunde ÄR crc-fältet.
    // Summan går därför över de sex första -- att ta sju räknade in crc:n i
    // sin egen kontrollsumma och kunde aldrig stämma.
    for (int i = 0; i < 6; i++) {
        checksum += raw_ptr[i];
    }
    checksum = ~checksum;

    return (header->header_crc == checksum);
}

// --- SYSTEMETS STARTPUNKT ---
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

    // 2. Aktivera avbrottsvektorn för systemanrop (från scheduler.S)
    extern void myrtos_trap_vector(void);
    // mtvec rörs inte: SDK:ns crt0 har redan satt den till sin vektortabell,
    // och våra hanterare har ersatt de svaga posterna vid länkningen. Tog vi
    // över den fungerade vår timer, men SDK:ns avbrottsregistrering slutade
    // fungera och TinyUSB assertade i dcd_init.
    // Bevisa att frigjort minne verkligen slås ihop igen. Blocken frigörs i en
    // ordning som kräver både framåt- och bakåtsammanslagning: mitten först,
    // så att den får två upptagna grannar, sedan den första och sist den sista.
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

    // Avbrott måste vara påslagna globalt innan USB startar; enskilda källor
    // slås på av den som behöver dem. Timern gör det annars först senare.
    __asm__ volatile("csrs mstatus, %0" : : "r"(1u << 3));

    myrtos_usb_init();

    // Bevisa trap-vägen innan något förlitar sig på den. Kommer vi tillbaka
    // hit har vektorn sparat, hanteraren kört, mepc stegats förbi ecall och
    // mret återvänt -- hela kedjan i ett anrop.
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

    // 3. Hitta modulen. Under QEMU la -device loader den på 0x80500000; på
    //    hårdvaran finns ingen sådan, så modulen följer med i flash tills den
    //    kan läsas från SD. Kärnan ser bara en pekare till ett modulhuvud, så
    //    bytet av källa senare rör ingenting nedanför den här raden.
    // --- MODULKATALOG OCH PROCESSER ----------------------------------------
    // Modulerna registreras en gång var. En process skapas sedan genom att
    // LÄNKA modulen, inte kopiera den: koden delas, bara dataområdet är privat.
    // Det är OS-9:s F$Link, och skälet till att systemet fick plats i 64 kB.
    extern void myrtos_scheduler_init(void);
    extern int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr, const char *args);
    extern void myrtos_timer_init(uint32_t);
    extern const uint8_t myrtos_embedded_shell_module[];

    myrtos_scheduler_init();
    myrtos_io_init();
    myrtos_moddir_init();

    // Flashen först: residenta moduler körs där de ligger och kostar inget
    // heapminne. Kortet får komplettera, och en modul med samma namn där
    // hamnar bredvid -- den som registrerades först vinner uppslagningen.
    myrtos_flash_scan();

    static uint8_t staging[32 * 1024];

    if (myrtos_sd_init() && myrtos_fat_mount()) {
        char name[12];
        for (uint32_t i = 0; myrtos_fat_find_nth("MOD", i, name); i++) {
            int32_t n = myrtos_fat_read_file(name, staging, sizeof(staging));
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

    if (!myrtos_moddir_count()) {
        myrtos_print("No modules found; registering the one built into the kernel.\n");
        // Den ligger i flash och kopieras inte -- resident, precis som OS-9:s
        // ROM-moduler, som kördes där de låg.
        myrtos_moddir_add_resident(
            (const myrtos_module_header_t*)myrtos_embedded_shell_module, "SHELL   MOD");
    }

    // Beskrivarna först: enheterna måste finnas innan någon process försöker
    // öppna dem. En datamodul har ingen startpunkt och startas inte.
    for (uint32_t i = 0; i < myrtos_moddir_count(); i++) {
        const myrtos_module_entry_t *e = myrtos_moddir_entry(i);
        if ((e->header->type_lang >> 8) != MYRTOS_TYPE_DATA) continue;
        myrtos_print("Descriptor ");
        myrtos_print(e->name);
        myrtos_print("\n");
        myrtos_io_add_descriptor(
            (const myrtos_descriptor_t*)((const uint8_t*)e->header + sizeof(myrtos_module_header_t)));
    }

    // Utan beskrivare finns inga enheter, och då kan ingen modul skriva något.
    // Kärnan faller tillbaka på den enhet den redan använder för sin egen
    // diagnostik, så systemet aldrig blir stumt bara för att kortet saknar en
    // beskrivare.
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

    // Finns ett skal startas bara det, och det startar resten på begäran.
    // Att starta allt vid uppstart var en demonstration, inte ett system.
    uint32_t started = 0;
    const char *shell = myrtos_moddir_match("sh");
    if (shell) {
        const myrtos_module_header_t *m = myrtos_moddir_link(shell);
        int32_t pid = m ? myrtos_process_create(m, "") : -1;
        if (pid >= 0) {
            // Ge skalet sina standardvägar. Allt det startar ärver dem, så
            // ett verktyg varken öppnar eller känner till någon enhet.
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
    }

    // Utan skal startas allt som finns, så systemet ändå visar livstecken.
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
        // TinyUSB gör sitt arbete här. wfi vore fel: enheten skulle bara
        // servas när något annat råkar väcka kärnan.
        myrtos_usb_task();
    }
}

// Pico SDK:s crt0 anropar main när klockor och runtime är uppsatta.
int main(void) {
    myrtos_uart_init();
    myrtos_kernel_main();
    return 0;
}
