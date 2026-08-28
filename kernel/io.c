#include "io.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "usbdev.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

// --- DRIVRUTIN: seriell terminal ------------------------------------------
// Drivrutinen bor ännu i kärnan, men den är inte längre hårdkodad mot en viss
// UART: beskrivaren säger vilken, på vilken pinne och i vilken takt. Nästa steg
// är att lyfta ut den som en egen modul på kortet.

static uart_inst_t *term_uart;
static uint32_t term_tx_pin;

static int32_t term_configure(const void *config, uint32_t size) {
    if (size < sizeof(myrtos_uart_config_t)) return -1;
    const myrtos_uart_config_t *c = (const myrtos_uart_config_t*)config;

    term_uart = (c->uart_base == 0x40070000u) ? uart0 : uart1;
    term_tx_pin = c->tx_pin;

    uart_init(term_uart, c->baud_rate);
    gpio_set_function(term_tx_pin, UART_FUNCSEL_NUM(term_uart, term_tx_pin));

    myrtos_print("  uart driver: base 0x");
    myrtos_print_hex(c->uart_base);      // print_u32 är decimal; hex behövs här
    myrtos_print(", tx GP");
    myrtos_print_u32(c->tx_pin);
    myrtos_print(", ");
    myrtos_print_u32(c->baud_rate);
    myrtos_print(" baud\n");
    return 0;
}

static int32_t term_open(void) { return term_uart ? 0 : -1; }
static int32_t term_close(void) { return 0; }

static int32_t term_write(const uint8_t *buf, uint32_t len) {
    // Anropas ur trap-hanteraren, alltså med avbrott avstängda. Hela
    // skrivningen blir därför odelbar utan något lås.
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n') uart_putc_raw(term_uart, '\r');
        uart_putc_raw(term_uart, (char)buf[i]);
    }
    return (int32_t)len;
}

// UART-mottagning: rx_pin sätts inte av beskrivaren än, så det finns inget att
// läsa. Funktionen finns för att gränssnittet ska vara komplett.
static int32_t term_read(uint8_t *buf, uint32_t len) {
    (void)buf; (void)len;
    return 0;
}

static const myrtos_driver_t driver_uart = {
    .module_name = "UART    MOD",
    .configure = term_configure,
    .open = term_open, .write = term_write, .read = term_read, .close = term_close
};

// --- DRIVRUTIN: USB CDC ---------------------------------------------------
// Ingen konfiguration behövs: identiteten sitter i USB-deskriptorerna, inte i
// enhetsbeskrivaren. Svansen får därför vara tom.

static int32_t usb_configure(const void *config, uint32_t size) {
    (void)config; (void)size;
    return 0;
}

static int32_t usb_open(void) { return 0; }
static int32_t usb_close(void) { return 0; }

static int32_t usb_write(const uint8_t *buf, uint32_t len) {
    // Ingen ansluten värd är inte ett fel: skrivningen kastas, precis som mot
    // en terminal ingen tittar på.
    int32_t n = myrtos_usb_write(buf, len);
    return n < 0 ? (int32_t)len : n;
}

static int32_t usb_read(uint8_t *buf, uint32_t len) {
    return myrtos_usb_read(buf, len);
}

static const myrtos_driver_t driver_usb = {
    .module_name = "USBCDC  MOD",
    .configure = usb_configure,
    .open = usb_open, .write = usb_write, .read = usb_read, .close = usb_close
};

static const myrtos_driver_t *drivers[MYRTOS_MAX_DRIVERS];
static uint32_t driver_count;

// --- ENHETER OCH VÄGAR ----------------------------------------------------
typedef struct {
    char name[12];
    const myrtos_driver_t *driver;
} myrtos_device_t;

// Vägnummer är PROCESSLOKALA, som i OS-9. Att de var globala gjorde att ett
// barn inte kunde ärva förälderns väg 0 -- numret var upptaget av någon annan.
#define MYRTOS_MAX_PROCS 8

typedef struct {
    const myrtos_device_t *device;
} myrtos_path_t;

static myrtos_device_t devices[MYRTOS_MAX_DEVICES];
static uint32_t device_count;
static myrtos_path_t paths[MYRTOS_MAX_PROCS][MYRTOS_MAX_PATHS];

static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static bool name11_eq(const char *a, const char *b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
    return true;
}

void myrtos_io_init(void) {
    driver_count = 0;
    drivers[driver_count++] = &driver_uart;
    drivers[driver_count++] = &driver_usb;
    device_count = 0;
    for (int p = 0; p < MYRTOS_MAX_PROCS; p++)
        for (int i = 0; i < MYRTOS_MAX_PATHS; i++)
            paths[p][i].device = 0;
    myrtos_print("I/O manager ready, awaiting device descriptors\n");
}

uint32_t myrtos_io_device_count(void) { return device_count; }

bool myrtos_io_add_descriptor(const myrtos_descriptor_t *desc) {
    if (device_count >= MYRTOS_MAX_DEVICES) return false;

    const myrtos_driver_t *drv = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        if (name11_eq(drivers[i]->module_name, desc->driver_name)) { drv = drivers[i]; break; }
    }
    if (!drv) {
        myrtos_print("  no driver named ");
        myrtos_print(desc->driver_name);
        myrtos_print("\n");
        return false;
    }

    // Konfigurationssvansen ligger direkt efter beskrivaren och tolkas bara av
    // drivrutinen; I/O-hanteraren vidarebefordrar den orörd.
    const uint8_t *base = (const uint8_t*)desc;
    if (drv->configure && desc->config_size) {
        if (drv->configure(base + desc->config_offset, desc->config_size) != 0) return false;
    }

    myrtos_device_t *d = &devices[device_count++];
    for (int i = 0; i < 11; i++) d->name[i] = desc->device_name[i];
    d->name[11] = 0;
    d->driver = drv;

    myrtos_print("  device '");
    myrtos_print(d->name);
    myrtos_print("' registered\n");
    return true;
}

int32_t myrtos_io_open(const char *name, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= MYRTOS_MAX_PROCS) return -1;
    for (uint32_t i = 0; i < device_count; i++) {
        if (!str_eq(devices[i].name, name)) continue;
        for (int p = 0; p < MYRTOS_MAX_PATHS; p++) {
            if (paths[owner_pid][p].device) continue;
            if (devices[i].driver->open() != 0) return -1;
            paths[owner_pid][p].device = &devices[i];
            return p;
        }
        return -1;
    }
    return -1;
}

int32_t myrtos_io_open_as(const char *name, int32_t owner_pid, int32_t path) {
    if (owner_pid < 0 || owner_pid >= MYRTOS_MAX_PROCS) return -1;
    if (path < 0 || path >= MYRTOS_MAX_PATHS) return -1;
    for (uint32_t i = 0; i < device_count; i++) {
        if (!str_eq(devices[i].name, name)) continue;
        if (devices[i].driver->open() != 0) return -1;
        paths[owner_pid][path].device = &devices[i];
        return path;
    }
    return -1;
}

// En process kan bara nå sina egna vägar: tabellen är indexerad på pid, så
// numret säger ingenting om någon annans.
static myrtos_path_t *path_of(int32_t path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= MYRTOS_MAX_PROCS) return 0;
    if (path < 0 || path >= MYRTOS_MAX_PATHS) return 0;
    if (!paths[owner_pid][path].device) return 0;
    return &paths[owner_pid][path];
}

// Ett barn ärver förälderns vägar med SAMMA nummer. Det är så ett verktyg kan
// skriva till väg 0 utan att veta vilken enhet skalet valde -- och därför
// mdir hamnar på USB när skalet gör det.
void myrtos_io_inherit(int32_t parent_pid, int32_t child_pid) {
    if (parent_pid < 0 || parent_pid >= MYRTOS_MAX_PROCS) return;
    if (child_pid < 0 || child_pid >= MYRTOS_MAX_PROCS) return;
    for (int i = 0; i < MYRTOS_MAX_PATHS; i++) {
        paths[child_pid][i] = paths[parent_pid][i];
    }
}

int32_t myrtos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid) {
    myrtos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    return p->device->driver->write(buf, len);
}

int32_t myrtos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid) {
    myrtos_path_t *p = path_of(path, owner_pid);
    if (!p || !p->device->driver->read) return -1;
    return p->device->driver->read(buf, len);
}

int32_t myrtos_io_close(int32_t path, int32_t owner_pid) {
    myrtos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    p->device->driver->close();
    p->device = 0;
    return 0;
}

void myrtos_io_close_all(int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= MYRTOS_MAX_PROCS) return;
    for (int i = 0; i < MYRTOS_MAX_PATHS; i++) {
        if (paths[owner_pid][i].device) {
            paths[owner_pid][i].device->driver->close();
            paths[owner_pid][i].device = 0;
        }
    }
}
