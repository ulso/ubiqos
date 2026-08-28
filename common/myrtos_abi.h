#ifndef MYRTOS_ABI_H
#define MYRTOS_ABI_H

#include <stdint.h>

// Gränssnittet mellan kärnan och modulerna. Allt som båda sidor måste vara
// överens om bor här och ingen annanstans -- modulformatet, systemanropens
// nummer och anropskonventionen.
//
// Numren låg tidigare i kärnan OCH i varje modul, skrivna för hand på tre
// ställen. En ändring i kärnan gav då inget byggfel utan ett "unknown system
// call" vid körning, vilket är precis den sortens tyst drift ett ABI finns för
// att förhindra.
//
// När det här slutar röra sig är den här filen vad ett myrtos-SDK består av:
// modulutveckling behöver den, inte kärnans källkod.

// --- FORMATVERSION --------------------------------------------------------
// Ligger i huvudets attr_rev, låga byten. Kärnan avvisar moduler byggda mot en
// annan version i stället för att köra dem och haverera på något obegripligt.
#define MYRTOS_ABI_VERSION 1

// --- MODULHUVUD -----------------------------------------------------------
#define MYRTOS_SYNC_CODE 0x0509000B

// Typ, i type_lang:s höga byte.
#define MYRTOS_TYPE_PROGRAM 1
#define MYRTOS_TYPE_DRIVER  2
#define MYRTOS_TYPE_DATA    3   // ingen kod, ingen startpunkt

// --- ENHETSBESKRIVARE -----------------------------------------------------
// En datamodul som beskriver en enhet, i OS-9:s mening. Den säger vad enheten
// heter, vilken drivrutinsmodul som hanterar den, och bär en svans som bara
// den drivrutinen förstår.
//
// Poängen är att en enhet ska kunna läggas till genom att släppa en fil på
// kortet, inte genom att bygga om kärnan. Vill man byta UART eller baudrate
// ändrar man beskrivaren.

#define MYRTOS_CLASS_CHAR   1   // teckenström: terminal, seriell port
#define MYRTOS_CLASS_BLOCK  2   // blockorienterad: SD, disk

typedef struct __attribute__((packed, aligned(4))) {
    char     device_name[12];   // det en process öppnar: "term"
    char     driver_name[12];   // modulen som hanterar den: "UART    MOD"
    uint16_t device_class;      // MYRTOS_CLASS_*
    uint16_t reserved;
    uint32_t config_offset;     // från beskrivarens början till svansen
    uint32_t config_size;       // svansens storlek, noll om ingen
} myrtos_descriptor_t;

// Svansen för UART-drivrutinen. Layouten är drivrutinens ensak; I/O-hanteraren
// vidarebefordrar den utan att tolka den.
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t uart_base;         // 0x40070000 för UART0 på RP2350
    uint32_t tx_pin;
    uint32_t rx_pin;            // 0xffffffff om enkelriktad
    uint32_t baud_rate;
} myrtos_uart_config_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t sync_code;      // MYRTOS_SYNC_CODE
    uint32_t module_size;    // hela modulen, huvudet inräknat
    uint32_t name_offset;    // till namnsträngen
    uint16_t type_lang;      // typ (program, drivrutin) och språk
    uint16_t attr_rev;       // attribut och ABI-version
    uint32_t exec_offset;    // till startpunkten
    uint32_t mem_size;       // RAM per process: data nedtill, stack från toppen
    uint32_t header_crc;     // komplementet av summan av de sex första orden
} myrtos_module_header_t;

// --- SYSTEMANROP ----------------------------------------------------------
// a7 bär numret, a0-a2 argumenten, a0 kommer tillbaka med resultatet.
#define SYS_NULL      0u   // gör ingenting; finns för att prova trap-vägen
#define SYS_IO_PUTC   1u   // a0 = tecken
#define SYS_EXIT      2u   // avslutar processen, återvänder aldrig
#define SYS_OPEN      3u   // a0 = enhetsnamn        -> a0 = vägnummer
#define SYS_WRITE     4u   // a0 = väg, a1 = buffert, a2 = längd
#define SYS_CLOSE     5u   // a0 = väg
#define SYS_MODDIR    6u   // a0 = index, a1 = buffert(12) -> a0 = länkar, -1 = slut
#define SYS_MEMINFO   7u   // a0 = 0 största fria block, 1 processer -> a0 = värde
#define SYS_READ      8u   // a0 = väg, a1 = buf, a2 = längd -> a0 = lästa, 0 = inget
#define SYS_EXEC      9u   // a0 = modulnamn, a1 = argumentsträng -> a0 = pid
#define SYS_ARGS     10u   // a0 = buffert, a1 = längd -> a0 = kopierade tecken

#define MYRTOS_MEM_LARGEST_FREE 0u
#define MYRTOS_MEM_PROCESSES    1u

// Anropet självt. Det är identiskt i varje modul, så det hör hemma här.
static inline int32_t myrtos_syscall(uint32_t id, uint32_t a, uint32_t b, uint32_t c) {
    register uint32_t r_id __asm__("a7") = id;
    register uint32_t r_a0 __asm__("a0") = a;
    register uint32_t r_a1 __asm__("a1") = b;
    register uint32_t r_a2 __asm__("a2") = c;

    __asm__ volatile (
        "ecall"
        : "+r"(r_a0)
        : "r"(r_id), "r"(r_a1), "r"(r_a2)
        : "memory"
    );
    return (int32_t)r_a0;
}

static inline int32_t myrtos_open(const char *device) {
    return myrtos_syscall(SYS_OPEN, (uint32_t)(uintptr_t)device, 0, 0);
}

static inline int32_t myrtos_write(int32_t path, const void *buf, uint32_t len) {
    return myrtos_syscall(SYS_WRITE, (uint32_t)path, (uint32_t)(uintptr_t)buf, len);
}

// Läsningen blockerar inte: noll betyder att ingenting fanns just nu. Kärnan
// har ännu inget sätt att sova en process på en enhet, så den som väntar på
// inmatning får fråga igen -- schemaläggaren tar ändå tillbaka tiden.
// Standardvägarna, samma konvention som OS-9 och Unix. Kärnan sätter upp dem
// åt den första processen och alla barn ärver dem, så ett verktyg varken
// öppnar eller stänger något: det läser väg 0 och skriver väg 1.
#define MYRTOS_STDIN  0
#define MYRTOS_STDOUT 1
#define MYRTOS_STDERR 2

// Ärvd utmatning om den finns, annars en egen konsol. Reserven behövs bara för
// en modul som startas utan förälder.
static inline int32_t myrtos_console(void) {
    if (myrtos_write(MYRTOS_STDOUT, "", 0) >= 0) return MYRTOS_STDOUT;
    int32_t p = myrtos_open("usb");
    if (p < 0) p = myrtos_open("term");
    return p;
}

static inline int32_t myrtos_read(int32_t path, void *buf, uint32_t len) {
    return myrtos_syscall(SYS_READ, (uint32_t)path, (uint32_t)(uintptr_t)buf, len);
}

// Starta en modul vid namn, med en kommandorad. OS-9:s F$Link följt av F$Fork.
static inline int32_t myrtos_exec(const char *module_name, const char *args) {
    return myrtos_syscall(SYS_EXEC, (uint32_t)(uintptr_t)module_name,
                          (uint32_t)(uintptr_t)args, 0);
}

// En modul startas som main: argc i a0, argv i a1, och argv[0] är modulens
// eget namn. Kärnan bygger vektorn i processens minne innan den startas.
//
//     void module_main(int argc, char **argv) { ... }
//
// En modul som inte bryr sig deklarerar module_main(void) som förut.

// Hämta den obearbetade kommandoraden. Finns kvar för den som hellre vill
// tolka den själv än gå genom argv.
static inline int32_t myrtos_args(char *buf, uint32_t len) {
    return myrtos_syscall(SYS_ARGS, (uint32_t)(uintptr_t)buf, len, 0);
}

static inline int32_t myrtos_close(int32_t path) {
    return myrtos_syscall(SYS_CLOSE, (uint32_t)path, 0, 0);
}

static inline void myrtos_exit(void) {
    myrtos_syscall(SYS_EXIT, 0, 0, 0);
}

// Bekvämlighet: skriv en nollterminerad sträng i ETT anrop. Att skicka hela
// strängen och inte ett tecken i taget är det som gör utskriften odelbar mot
// andra processer.
// --- FUNKTIONSTABELLER I FLYTTBAR KOD -------------------------------------
// En vanlig tabell av funktionspekare bär absoluta adresser som länkaren
// skrivit in, och bryter positionsoberoendet. Lagrar man i stället AVSTÅNDET
// från tabellen till funktionen, och adderar tabellens adress vid körning, blir
// tabellen flyttbar -- och kan ligga const i .rodata, alltså delad mellan
// processer. En tabell byggd på stacken vid körning fungerar också, men då får
// varje process sin egen kopia.
//
// Differensen måste räknas ut av assemblern: C avvisar den som initierare,
// eftersom skillnaden mellan två adresser inte är en konstant i språkets
// mening. Resultatet blir ADD32/SUB32-relokeringar, som är länkningskonstanter
// utan absolut adress.
//
// Funktionerna måste bära __attribute__((used)): de refereras bara från
// assembler, som kompilatorn inte ser, och optimeras annars bort som oanvända.
//
//     __attribute__((used)) static void do_read(int a) { ... }
//     MYRTOS_RELTAB_BEGIN(ops);
//     MYRTOS_RELTAB_ENTRY(ops, do_read);
//     MYRTOS_RELTAB_ENTRY(ops, do_write);
//     MYRTOS_RELTAB_END();
//     ...
//     MYRTOS_RELTAB_CALL(ops, i, void (*)(int))(arg);

#define MYRTOS_RELTAB_BEGIN(name)                                  \
    extern const intptr_t name[];                                  \
    __asm__(".pushsection .rodata." #name ",\"a\"\n"               \
            ".balign 4\n.globl " #name "\n" #name ":")

#define MYRTOS_RELTAB_ENTRY(name, fn)                              \
    __asm__(".word " #fn " - " #name)

// .popsection är inte valfri: utan den ligger sektionsbytet kvar och all kod
// som följer hamnar i tabellens sektion i stället för i .text.
#define MYRTOS_RELTAB_END()  __asm__(".popsection")

#define MYRTOS_RELTAB_CALL(name, index, type)                      \
    ((type)((intptr_t)(name) + (name)[index]))

// En skrivning är odelbar, men en RAD är det bara om den skrivs i ett anrop.
// Bygger ett verktyg sin rad av flera skrivningar hinner andra processer
// emellan, och utskriften blir oläslig så fort mer än en process talar. Därför
// den här: samla raden, skicka den en gång.
typedef struct {
    char buf[96];
    uint32_t len;
} myrtos_line_t;

static inline void myrtos_line_reset(myrtos_line_t *l) { l->len = 0; }

static inline void myrtos_line_str(myrtos_line_t *l, const char *s) {
    while (*s && l->len < sizeof(l->buf) - 1) l->buf[l->len++] = *s++;
}

static inline void myrtos_line_chars(myrtos_line_t *l, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n && l->len < sizeof(l->buf) - 1; i++) l->buf[l->len++] = s[i];
}

static inline void myrtos_line_u32(myrtos_line_t *l, uint32_t v) {
    char tmp[11];
    int i = 10;
    tmp[i] = 0;
    if (!v) tmp[--i] = '0';
    while (v) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    myrtos_line_str(l, &tmp[i]);
}

// Verktyg behöver skriva tal, och en modul har ingen printf. Tio rader här
// sparar dem i varje utility.
static inline int32_t myrtos_write_u32(int32_t path, uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    return myrtos_write(path, &buf[i], (uint32_t)(10 - i));
}

static inline int32_t myrtos_moddir_get(uint32_t index, char *name_out) {
    return myrtos_syscall(SYS_MODDIR, index, (uint32_t)(uintptr_t)name_out, 0);
}

static inline int32_t myrtos_meminfo(uint32_t what) {
    return myrtos_syscall(SYS_MEMINFO, what, 0, 0);
}

static inline int32_t myrtos_line_flush(int32_t path, myrtos_line_t *l) {
    int32_t r = myrtos_write(path, l->buf, l->len);
    l->len = 0;
    return r;
}

static inline int32_t myrtos_write_str(int32_t path, const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return myrtos_write(path, s, n);
}

#endif
