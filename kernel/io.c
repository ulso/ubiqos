#include "io.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "usbdev.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);

// --- DRIVER: serial terminal ----------------------------------------------
// The driver still lives in the kernel, but it is no longer hardcoded to one
// UART: the descriptor says which, on which pin and at what rate. The next step
// is to lift it out as a module of its own on the card.

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
    myrtos_print_hex(c->uart_base);      // print_u32 is decimal; hex is wanted here
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
    // Called from the trap handler, hence with interrupts off. The whole
    // write is therefore atomic without any lock.
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n') uart_putc_raw(term_uart, '\r');
        uart_putc_raw(term_uart, (char)buf[i]);
    }
    return (int32_t)len;
}

// UART receive: the descriptor does not set rx_pin yet, so there is nothing to
// read. The function exists to keep the interface complete.
static int32_t term_read(uint8_t *buf, uint32_t len) {
    (void)buf; (void)len;
    return 0;
}

static const myrtos_driver_t driver_uart = {
    .module_name = "UART    MOD",
    .configure = term_configure,
    .open = term_open, .write = term_write, .read = term_read, .close = term_close
};

// --- DRIVER: USB CDC ------------------------------------------------------
// No configuration is needed: the identity sits in the USB descriptors, not in
// the device descriptor. The tail may therefore be empty.

static int32_t usb_configure(const void *config, uint32_t size) {
    (void)config; (void)size;
    return 0;
}

static int32_t usb_open(void) { return 0; }
static int32_t usb_close(void) { return 0; }

static int32_t usb_write(const uint8_t *buf, uint32_t len) {
    // No attached host is not an error: the write is dropped, exactly as it
    // would be to a terminal nobody is watching.
    int32_t n = myrtos_usb_write(buf, len);
    return n < 0 ? (int32_t)len : n;
}

static int32_t usb_read(uint8_t *buf, uint32_t len) {
    return myrtos_usb_read(buf, len);
}

static int32_t usb_readable(void) {
    return (int32_t)myrtos_usb_available();
}

static const myrtos_driver_t driver_usb = {
    .module_name = "USBCDC  MOD",
    .configure = usb_configure,
    .open = usb_open, .write = usb_write, .read = usb_read, .close = usb_close,
    .readable = usb_readable
};

static const myrtos_driver_t *drivers[MYRTOS_MAX_DRIVERS];
static uint32_t driver_count;

// --- DEVICES AND PATHS ----------------------------------------------------
typedef struct {
    char name[12];
    const myrtos_driver_t *driver;
} myrtos_device_t;

// Path numbers are PER-PROCESS, as in OS-9. Being global meant a child could
// not inherit its parent's path 0 -- the number was taken by someone else.
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

    // The configuration tail sits right after the descriptor and is read only
    // by the driver; the I/O manager passes it on untouched.
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

// A process can only reach its own paths: the table is indexed by pid, so the
// number says nothing about anyone else's.
static myrtos_path_t *path_of(int32_t path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= MYRTOS_MAX_PROCS) return 0;
    if (path < 0 || path >= MYRTOS_MAX_PATHS) return 0;
    if (!paths[owner_pid][path].device) return 0;
    return &paths[owner_pid][path];
}

// A child inherits its parent's paths under the SAME numbers. That is how a
// utility can write to path 1 without knowing which device the shell chose --
// and why a utility's output lands on USB when the shell is there.
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

bool myrtos_io_readable(int32_t path, int32_t owner_pid) {
    myrtos_path_t *p = path_of(path, owner_pid);
    if (!p || !p->device->driver->readable) return false;
    return p->device->driver->readable() > 0;
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
