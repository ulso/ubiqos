#include "io.h"
#include "keystore.h"
#include "critical.h"
#include "tusb.h"          // the CDC host class, for the acm driver
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "usbdev.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);

// --- DRIVER: serial terminal ----------------------------------------------
// It was here, and it is modules/uartdrv now -- the first driver to be a module
// of its own, which is what the note that stood here said was the next step.
// The descriptor already said which UART, on which pin, at what rate; the only
// thing still compiled in was the code that read it.
//
// Nothing about the device changed: /dev/term is registered from the same
// descriptor, by the same ubiqos_io_add_descriptor, through the same vtable.
// What changed is where the vtable comes from.

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
    int32_t n = ubiqos_usb_write(buf, len);
    return n < 0 ? (int32_t)len : n;
}

static int32_t usb_read(uint8_t *buf, uint32_t len) {
    return ubiqos_usb_read(buf, len);
}

static int32_t usb_readable(void) {
    return (int32_t)ubiqos_usb_available();
}

// --- DRIVER: USB keyboard -------------------------------------------------
// Read only, and no configuration: which pins the host uses is the host's
// business, and there is only one of it. What makes this a device rather than a
// special case is that a process opens it by name and reads it like any other.
int32_t ubiqos_usbhost_read(uint8_t *buf, uint32_t len);
uint32_t ubiqos_usbhost_available(void);

void ubiqos_usbhost_set_keymap(const ubiqos_keymap_t *k);

// The layout arrives with the descriptor, like the UART's pins and baud rate.
static int32_t kbd_configure(const void *config, uint32_t size) {
    if (size < sizeof(ubiqos_keymap_t)) return -1;
    ubiqos_usbhost_set_keymap((const ubiqos_keymap_t*)config);
    return 0;
}

static int32_t kbd_open(void)  { return 0; }
static int32_t kbd_close(void) { return 0; }
static int32_t kbd_write(const uint8_t *buf, uint32_t len) {
    (void)buf; (void)len;
    return -1;                      // a keyboard has nothing to say back
}
static int32_t kbd_read(uint8_t *buf, uint32_t len) {
    return ubiqos_usbhost_read(buf, len);
}
static int32_t kbd_readable(void) {
    return (int32_t)ubiqos_usbhost_available();
}

// --- DRIVER: CDC-ACM ON THE HOST SIDE -------------------------------------
// A serial port at the other end of the USB socket rather than at the other end
// of the cable to the Mac. It behaves like every other character device here:
// a process opens it by name and reads and writes it, and neither knows nor
// cares that a class driver and a hub are in the way.
//
// Nothing is buffered on this side. TinyUSB keeps a packet each way and the USB
// thread empties it every millisecond, which is far quicker than a shell reads.
int32_t ubiqos_usbhost_cdc_index(void);

static int32_t acm_configure(const void *config, uint32_t size) {
    (void)config; (void)size;
    return 0;                       // which device is plugged in is not our choice
}

static int32_t acm_open(void)  { return 0; }
static int32_t acm_close(void) { return 0; }

// Absent is an error, and saying so is worth the small inconvenience. It used to
// claim the write had gone out, on the theory that a port with nothing plugged
// in is like a terminal nobody is watching -- but a terminal nobody is watching
// still exists, and this does not. What it bought was an afternoon of a program
// that said "connected" and swallowed everything in silence.
static int32_t acm_write(const uint8_t *buf, uint32_t len) {
    int32_t idx = ubiqos_usbhost_cdc_index();
    if (idx < 0 || !tuh_cdc_mounted((uint8_t)idx)) return -1;
    uint32_t room = tuh_cdc_write_available((uint8_t)idx);
    if (len > room) len = room;
    if (!len) return 0;
    uint32_t n = tuh_cdc_write((uint8_t)idx, buf, len);
    tuh_cdc_write_flush((uint8_t)idx);
    return (int32_t)n;
}

static int32_t acm_read(uint8_t *buf, uint32_t len) {
    int32_t idx = ubiqos_usbhost_cdc_index();
    if (idx < 0 || !tuh_cdc_mounted((uint8_t)idx)) return 0;
    return (int32_t)tuh_cdc_read((uint8_t)idx, buf, len);
}

static int32_t acm_readable(void) {
    int32_t idx = ubiqos_usbhost_cdc_index();
    if (idx < 0 || !tuh_cdc_mounted((uint8_t)idx)) return 0;
    return (int32_t)tuh_cdc_read_available((uint8_t)idx);
}

static int32_t acm_writable(void) {
    int32_t idx = ubiqos_usbhost_cdc_index();
    if (idx < 0 || !tuh_cdc_mounted((uint8_t)idx)) return 1;   // swallowed, not blocked
    return (int32_t)tuh_cdc_write_available((uint8_t)idx);
}

static const ubiqos_driver_t driver_acm = {
    .module_name = "acm",
    .configure = acm_configure,
    .open = acm_open, .write = acm_write, .read = acm_read, .close = acm_close,
    .readable = acm_readable, .writable = acm_writable
};

// The console: the display to write to, the keyboard to read from. Output never
// blocks -- a screen is always ready -- so writable reports plenty of room.
void ubiqos_console_putc(char c);

static int32_t con_open(void)  { return 0; }
static int32_t con_close(void) { return 0; }
// The write only copies. A kernel thread draws, in process context, where it can
// be preempted -- see console.c. Returning a short count is the contract, and
// returning zero blocks the caller on WAIT_WRITE like any other full device.
uint32_t ubiqos_console_put(const uint8_t *buf, uint32_t len);
uint32_t ubiqos_console_room(void);

static int32_t con_write(const uint8_t *buf, uint32_t len) {
    return (int32_t)ubiqos_console_put(buf, len);
}
static int32_t con_writable(void) { return (int32_t)ubiqos_console_room(); }

static int32_t con_read(uint8_t *buf, uint32_t len) {
    return ubiqos_usbhost_read(buf, len);
}
static int32_t con_readable(void) {
    return (int32_t)ubiqos_usbhost_available();
}

// /dev/null. Everything written to it is taken and forgotten, and reading it is
// immediately the end -- which is the part that needs saying, since a read of
// nothing means "not yet" everywhere else here and a reader would wait for ever.
static int32_t null_open(void)  { return 0; }
static int32_t null_close(void) { return 0; }
static int32_t null_write(const uint8_t *buf, uint32_t len) { (void)buf; return (int32_t)len; }
static int32_t null_read(uint8_t *buf, uint32_t len) { (void)buf; (void)len; return 0; }
static int32_t null_readable(void) { return 1; }        // the end is always ready
static int32_t null_at_eof(void)   { return 1; }

static const ubiqos_driver_t driver_null = {
    .module_name = "null",
    .configure = 0,
    .open = null_open, .write = null_write, .read = null_read, .close = null_close,
    .readable = null_readable, .at_eof = null_at_eof
};

static const ubiqos_driver_t driver_console = {
    .module_name = "console",
    .configure = 0,
    .open = con_open, .write = con_write, .read = con_read, .close = con_close,
    .readable = con_readable, .writable = con_writable
};

static const ubiqos_driver_t driver_kbd = {
    .module_name = "usbkbd",
    .configure = kbd_configure,
    .open = kbd_open, .write = kbd_write, .read = kbd_read, .close = kbd_close,
    .readable = kbd_readable
};

static int32_t usb_writable(void) {
    return (int32_t)ubiqos_usb_writable();
}

static const ubiqos_driver_t driver_usb = {
    .module_name = "usbcdc",
    .configure = usb_configure,
    .open = usb_open, .write = usb_write, .read = usb_read, .close = usb_close,
    .readable = usb_readable, .writable = usb_writable
};

static const ubiqos_driver_t *drivers[UBIQOS_MAX_DRIVERS];
static uint32_t driver_count;

// --- DRIVERS THAT ARE MODULES ---------------------------------------------
// Linked on demand, when a descriptor names a driver the kernel was not built
// with. The table it returns is inside the relocated copy in PSRAM and stays
// valid for as long as the module is linked, which is for ever: nothing
// unlinks a driver, because a device once registered is registered.
//
// Registering the same driver twice would link the module twice and give the
// two devices separate copies of its state, so a driver that has been linked is
// remembered in drivers[] alongside the compiled-in ones and found by the
// ordinary search on the next descriptor.
const ubiqos_driver_module_t *ubiqos_driver_link(const char *name, void **owned_out);
extern const ubiqos_kernel_api_t ubiqos_kernel_api;

static const ubiqos_driver_t *driver_from_module(const char *name)
{
    // Said out loud. Returning zero here reads to the caller exactly like "no
    // such driver", and the message it prints then sends the reader looking
    // for a missing module rather than a full table.
    if (driver_count >= UBIQOS_MAX_DRIVERS) {
        ubiqos_print("  no room for another driver (UBIQOS_MAX_DRIVERS)\n");
        return 0;
    }

    const ubiqos_driver_module_t *m = ubiqos_driver_link(name, 0);
    if (!m) return 0;
    if (!m->init || !m->init(&ubiqos_kernel_api)) {
        ubiqos_print("  driver ");
        ubiqos_print(name);
        ubiqos_print(" would not start\n");
        return 0;
    }

    ubiqos_print("  driver '");
    ubiqos_print(name);
    ubiqos_print("' linked, running from the module pool\n");
    drivers[driver_count++] = &m->ops;
    return &m->ops;
}

// --- DEVICES AND PATHS ----------------------------------------------------
typedef struct {
    char name[UBIQOS_NAME_LEN];
    const ubiqos_driver_t *driver;
    // The process this device's interrupt key should end. A terminal has one:
    // the command running in front of it, which is emphatically not the process
    // reading the keyboard -- while a command runs, nobody is reading. The
    // shell sets it, because the shell is the only thing that knows what it
    // started and on which device.
    int32_t foreground;
} ubiqos_device_t;

// Path numbers are PER-PROCESS, as in OS-9. Being global meant a child could
// not inherit its parent's path 0 -- the number was taken by someone else.
//
// The table is indexed by pid, so it must be as tall as the scheduler's process
// table. One definition, shared, rather than two numbers that have to agree.
#define UBIQOS_MAX_PROCS UBIQOS_MAX_PROCESSES

typedef struct {
    const ubiqos_device_t *device;
    int16_t file;                  // index into open_files, -1 for a device
    int16_t pipe;                  // index into pipes, -1 when not one
    uint8_t pipe_write;            // which end of it this descriptor is
} ubiqos_path_t;

// See io.h. A ring, and the counts of who still holds each end -- the second is
// what tells an empty pipe apart from a finished one.
typedef struct {
    uint8_t  buf[UBIQOS_PIPE_BUF];
    uint32_t head, tail;
    int32_t  readers, writers;
} pipe_t;

static pipe_t pipes[UBIQOS_MAX_PIPES];

static uint32_t pipe_used(const pipe_t *q) {
    return (q->head - q->tail) % UBIQOS_PIPE_BUF;
}

static ubiqos_path_t *pipe_entry(int32_t path, int32_t owner_pid);
static void pipe_release(ubiqos_path_t *p);

// See io.h. Written by the filesystem server, which is a thread, and read and
// written by open and close, which are traps -- so every one of them holds
// interrupts, for the same reason the allocator does.
typedef struct {
    char     path[64];
    uint32_t pos;
    int32_t  refs;                 // a child inherits the position, as fork does
} open_file_t;

static open_file_t open_files[UBIQOS_MAX_OPEN_FILES];

static ubiqos_device_t devices[UBIQOS_MAX_DEVICES];
static uint32_t device_count;
static ubiqos_path_t paths[UBIQOS_MAX_PROCS][UBIQOS_MAX_PATHS];

static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// A descriptor names its driver, and the two are equal when they are the same
// string. This used to compare exactly eleven characters, so every descriptor
// in the tree wrote the padded 8.3 form by hand -- "uart", four spaces,
// counted by a human -- and getting the count wrong produced a device that
// registered and then had no driver.
static bool name_eq_ci(const char *a, const char *b) {
    for (;;) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
        a++; b++;
    }
}

void ubiqos_io_init(void) {
    driver_count = 0;
    drivers[driver_count++] = &driver_usb;
    drivers[driver_count++] = &driver_kbd;
    drivers[driver_count++] = &driver_console;
    drivers[driver_count++] = &driver_acm;
    drivers[driver_count++] = &driver_null;
    device_count = 0;
    for (int p = 0; p < UBIQOS_MAX_PROCS; p++) {
        for (int i = 0; i < UBIQOS_MAX_PATHS; i++) {
            paths[p][i].device = 0;
            paths[p][i].file = -1;
            paths[p][i].pipe = -1;
        }
    }
    // null needs no descriptor. A descriptor says which pins, which speed and
    // which driver; this one has no hardware to describe, and a data module
    // holding nothing but its own name would be ceremony rather than
    // configuration. So it is registered here, and it is the only one.
    ubiqos_device_t *n = &devices[device_count++];
    const char *nm = "null";
    // NUL-filled, as a descriptor's name arrives: the lookup compares C strings,
    // and a name padded with spaces would never match what anyone types.
    int k = 0;
    while (nm[k]) { n->name[k] = nm[k]; k++; }
    while (k < 12) n->name[k++] = 0;
    n->driver = &driver_null;
    n->foreground = -1;

    ubiqos_print("I/O manager ready, awaiting device descriptors\n");
}

uint32_t ubiqos_io_device_count(void) { return device_count; }

// The nth device's name, so /dev can list what is registered. The order is
// registration order and nothing more; nobody should depend on it.
bool ubiqos_io_device_nth(uint32_t index, char *name_out) {
    if (index >= device_count) return false;
    int i = 0;
    while (i < UBIQOS_NAME_LEN - 1 && devices[index].name[i]) { name_out[i] = devices[index].name[i]; i++; }
    name_out[i] = 0;
    return true;
}

// Whether a named device was registered. Asked before starting a process that
// would have nowhere to talk: there is no way to kill another process, so the
// question has to come first.
bool ubiqos_io_has_device(const char *name) {
    for (uint32_t i = 0; i < device_count; i++)
        if (str_eq(devices[i].name, name)) return true;
    return false;
}

bool ubiqos_io_add_descriptor(const ubiqos_descriptor_t *desc) {
    // Also said out loud. This returned false in silence, and a device that
    // never appears in /dev with nothing printed anywhere is a thing you find
    // by counting the entries and wondering.
    if (device_count >= UBIQOS_MAX_DEVICES) {
        ubiqos_print("  no room for device '");
        ubiqos_print(desc->device_name);
        ubiqos_print("' (UBIQOS_MAX_DEVICES)\n");
        return false;
    }

    const ubiqos_driver_t *drv = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        if (name_eq_ci(drivers[i]->module_name, desc->driver_name)) { drv = drivers[i]; break; }
    }
    // Not compiled in, so look for a module of that name. This is the half of
    // the OS-9 model that was missing: a descriptor has always been a file, and
    // now so is the driver it names. A board can be given a device it was never
    // built with by dropping two files on the card.
    if (!drv) drv = driver_from_module(desc->driver_name);
    if (!drv) {
        ubiqos_print("  no driver named ");
        ubiqos_print(desc->driver_name);
        ubiqos_print("\n");
        return false;
    }

    // The configuration tail sits right after the descriptor and is read only
    // by the driver; the I/O manager passes it on untouched.
    //
    // Called even when there is no tail, which it was not. "Configure" is the
    // moment a driver gets ready, and a driver can have nothing to be told and
    // still have work to do: the neopixel driver has one strip on one pin and
    // no configuration at all, and it needs that call to set up its PIO. It
    // registered, appeared in /dev, and refused every open -- because the one
    // function that would have made it ready was skipped for having nothing to
    // read.
    //
    // Safe for the drivers that were already here: the ones with no
    // configuration leave the pointer null, and the ones that have a function
    // either ignore both arguments or are named by a descriptor that does carry
    // a tail.
    const uint8_t *base = (const uint8_t*)desc;
    if (drv->configure) {
        const void *tail = desc->config_size ? base + desc->config_offset : 0;
        if (drv->configure(tail, desc->config_size) != 0) return false;
    }

    ubiqos_device_t *d = &devices[device_count++];
    int i = 0;
    while (i < UBIQOS_NAME_LEN - 1 && desc->device_name[i]) { d->name[i] = desc->device_name[i]; i++; }
    d->name[i] = 0;
    d->driver = drv;
    d->foreground = -1;

    ubiqos_print("  device '");
    ubiqos_print(d->name);
    ubiqos_print("' registered\n");
    return true;
}

int32_t ubiqos_io_open(const char *name, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return -1;
    for (uint32_t i = 0; i < device_count; i++) {
        if (!str_eq(devices[i].name, name)) continue;
        for (int p = 0; p < UBIQOS_MAX_PATHS; p++) {
            // A slot is free when it holds NOTHING. Testing only for a device
            // was the bug: a descriptor onto a file has no device, so opening a
            // device took the slot a redirected stdout was sitting in and wrote
            // over it. "hibouair > /tmp/f" then had its own dongle on
            // descriptor 1, so every line it printed went out to the BleuIO --
            // no file was created, and nothing appeared on the console either,
            // which is exactly what it looked like from outside.
            //
            // ubiqos_io_open_file had it right all along, and the two now agree.
            if (paths[owner_pid][p].device) continue;
            if (paths[owner_pid][p].file >= 0) continue;
            if (paths[owner_pid][p].pipe >= 0) continue;
            if (devices[i].driver->open && devices[i].driver->open() != 0) return -1;
            paths[owner_pid][p].device = &devices[i];
            paths[owner_pid][p].file = -1;
            paths[owner_pid][p].pipe = -1;
            return p;
        }
        return -1;
    }
    return -1;
}

int32_t ubiqos_io_open_as(const char *name, int32_t owner_pid, int32_t path) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return -1;
    if (path < 0 || path >= UBIQOS_MAX_PATHS) return -1;
    for (uint32_t i = 0; i < device_count; i++) {
        if (!str_eq(devices[i].name, name)) continue;
        if (devices[i].driver->open && devices[i].driver->open() != 0) return -1;
        paths[owner_pid][path].device = &devices[i];
        return path;
    }
    return -1;
}

// A process can only reach its own paths: the table is indexed by pid, so the
// number says nothing about anyone else's.
// A file entry, or null. Deliberately separate from path_of, which answers for
// devices only: every existing caller of that means "a device" and would be
// wrong about a file.
static ubiqos_path_t *file_entry(int32_t path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return 0;
    if (path < 0 || path >= UBIQOS_MAX_PATHS) return 0;
    if (paths[owner_pid][path].file < 0) return 0;
    return &paths[owner_pid][path];
}

// A descriptor slot nobody is using.
//
// A PIPE END COUNTS AS USED, and did not until now: the two tests this
// replaces looked for a slot with no device and no file, and a slot holding a
// pipe has neither. So the first free descriptor found after a pipe() was the
// pipe's own, and dup(fd, -1) quietly wrote over it. The program that found
// this was netcon, which makes two pipes and then saves its own stdin: the
// save landed on the pipe's read end, the child inherited the console instead,
// and a shell that answered on the network took its keystrokes from the serial
// port. Nothing said anything -- the counts for the overwritten end were never
// given back either.
static bool is_free(const ubiqos_path_t *p) {
    return !p->device && p->file < 0 && p->pipe < 0;
}

int32_t ubiqos_io_open_file(const char *abs_path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS || !abs_path) return -1;
    uint32_t st = ubiqos_critical_enter();

    int32_t slot = -1;
    for (int i = 0; i < UBIQOS_MAX_OPEN_FILES; i++)
        if (!open_files[i].refs) { slot = i; break; }
    int32_t fd = -1;
    for (int i = 0; i < UBIQOS_MAX_PATHS; i++)
        if (is_free(&paths[owner_pid][i])) { fd = i; break; }
    if (slot < 0 || fd < 0) { ubiqos_critical_exit(st); return -1; }

    uint32_t n = 0;
    while (abs_path[n] && n < sizeof(open_files[0].path) - 1) {
        open_files[slot].path[n] = abs_path[n];
        n++;
    }
    open_files[slot].path[n] = 0;
    open_files[slot].pos = 0;
    open_files[slot].refs = 1;
    paths[owner_pid][fd].file = (int16_t)slot;

    ubiqos_critical_exit(st);
    return fd;
}

bool ubiqos_io_is_file(int32_t path, int32_t owner_pid) {
    return file_entry(path, owner_pid) != 0;
}

bool ubiqos_io_file_at(int32_t path, int32_t owner_pid,
                       const char **path_out, uint32_t *pos_out) {
    ubiqos_path_t *p = file_entry(path, owner_pid);
    if (!p) return false;
    if (path_out) *path_out = open_files[p->file].path;
    if (pos_out) *pos_out = open_files[p->file].pos;
    return true;
}

void ubiqos_io_file_advance(int32_t path, int32_t owner_pid, uint32_t n) {
    uint32_t st = ubiqos_critical_enter();
    ubiqos_path_t *p = file_entry(path, owner_pid);
    if (p) open_files[p->file].pos += n;
    ubiqos_critical_exit(st);
}

int32_t ubiqos_io_file_seek(int32_t path, int32_t owner_pid,
                            int32_t offset, uint32_t whence) {
    uint32_t st = ubiqos_critical_enter();
    ubiqos_path_t *p = file_entry(path, owner_pid);
    int32_t result = -1;
    if (p) {
        uint32_t pos = open_files[p->file].pos;
        bool ok = true;
        switch (whence) {
        case UBIQOS_SEEK_SET: pos = (uint32_t)offset; break;
        case UBIQOS_SEEK_CUR: pos = (uint32_t)((int32_t)pos + offset); break;
        // SEEK_END never reaches here. It needs the file's length, which is
        // the filesystem's to give and long work to ask for, so the trap sends
        // it to the server and the server comes back through SEEK_SET. Anything
        // else is a caller mistake, and refusing beats seeking somewhere
        // plausible.
        default: ok = false; break;
        }
        if (ok) {
            open_files[p->file].pos = pos;
            result = (int32_t)pos;
        }
    }
    ubiqos_critical_exit(st);
    return result;
}

// Dropping one reference to an open file. The slot goes when the last
// descriptor on it does, which is what makes a child's copy safe.
static void file_release(ubiqos_path_t *p) {
    if (p->file < 0) return;
    if (open_files[p->file].refs > 0) open_files[p->file].refs--;
    p->file = -1;
}

static ubiqos_path_t *path_of(int32_t path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return 0;
    if (path < 0 || path >= UBIQOS_MAX_PATHS) return 0;
    if (!paths[owner_pid][path].device) return 0;
    return &paths[owner_pid][path];
}

// A child inherits its parent's paths under the SAME numbers. That is how a
// utility can write to path 1 without knowing which device the shell chose --
// and why a utility's output lands on USB when the shell is there.
void ubiqos_io_inherit(int32_t parent_pid, int32_t child_pid) {
    if (parent_pid < 0 || parent_pid >= UBIQOS_MAX_PROCS) return;
    if (child_pid < 0 || child_pid >= UBIQOS_MAX_PROCS) return;
    uint32_t st = ubiqos_critical_enter();
    for (int i = 0; i < UBIQOS_MAX_PATHS; i++) {
        paths[child_pid][i] = paths[parent_pid][i];
        // The child shares the open file, position and all, exactly as a fork's
        // descriptors do. Two processes appending to the same file interleave
        // rather than overwrite, which is the whole point of sharing it.
        if (paths[child_pid][i].file >= 0) open_files[paths[child_pid][i].file].refs++;
        // A pipe end held by two processes is what makes a pipeline work: the
        // writer's end is not finished until every holder has let go.
        if (paths[child_pid][i].pipe >= 0) {
            if (paths[child_pid][i].pipe_write) pipes[paths[child_pid][i].pipe].writers++;
            else                                pipes[paths[child_pid][i].pipe].readers++;
        }
    }
    ubiqos_critical_exit(st);
}

int32_t ubiqos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid) {
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    if (q) {
        if (!q->pipe_write) return -1;                  // the wrong end
        uint32_t st = ubiqos_critical_enter();
        pipe_t *r = &pipes[q->pipe];
        // Nobody to read it. Failing beats filling a buffer that will never be
        // emptied, which is what SIGPIPE is for elsewhere.
        if (!r->readers) { ubiqos_critical_exit(st); return -1; }
        uint32_t n = 0;
        while (n < len && pipe_used(r) < UBIQOS_PIPE_BUF - 1) {
            r->buf[r->head] = buf[n++];
            r->head = (r->head + 1) % UBIQOS_PIPE_BUF;
        }
        ubiqos_critical_exit(st);
        return (int32_t)n;                              // zero means wait for room
    }
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    return p->device->driver->write(buf, len);
}

// --- STATUS ---------------------------------------------------------------
// A device is asked about itself, or told something, without that going through
// its data. Both refuse a pipe: a pipe has no device behind it and nothing to
// answer with.

int32_t ubiqos_io_getstat(int32_t path, uint32_t code, void *data, uint32_t len,
                          int32_t owner_pid)
{
    if (pipe_entry(path, owner_pid)) return -1;
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p || !p->device->driver->getstat) return -1;
    return p->device->driver->getstat(code, data, len);
}

int32_t ubiqos_io_setstat(int32_t path, uint32_t code, const void *data, uint32_t len,
                          int32_t owner_pid)
{
    if (pipe_entry(path, owner_pid)) return -1;
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p || !p->device->driver->setstat) return -1;

    // "Join this network with the password you are keeping for it." The
    // password goes from the key store to the driver without leaving the
    // kernel: a process asks by name and never sees the bytes, which is the
    // whole point of the store. Anything else goes straight through.
    if (code == UBIQOS_SS_EH_JOIN_KEY) {
        const char *ssid = data;
        if (!ssid || !len || !ssid[0]) return -1;

        char name[UBIQOS_KEY_NAME_MAX];
        uint32_t n = 0;
        for (const char *k = "wifi."; *k; k++) name[n++] = *k;
        for (uint32_t i = 0; i < len && ssid[i] && n < sizeof name - 1; i++) name[n++] = ssid[i];
        name[n] = 0;

        uint32_t plen = 0;
        const uint8_t *pass = ubiqos_keys_value(name, &plen);
        if (!pass) return -2;                   // no such key, or locked

        char creds[UBIQOS_KEY_NAME_MAX + UBIQOS_KEY_VALUE_MAX + 2];
        uint32_t c = 0;
        for (uint32_t i = 0; i < len && ssid[i] && c < sizeof creds - 2; i++) creds[c++] = ssid[i];
        creds[c++] = 0;
        for (uint32_t i = 0; i < plen && c < sizeof creds - 1; i++) creds[c++] = (char)pass[i];
        creds[c++] = 0;

        const int32_t rc = p->device->driver->setstat(UBIQOS_SS_EH_JOIN, creds, c);
        for (uint32_t i = 0; i < sizeof creds; i++) creds[i] = 0;
        return rc;
    }

    return p->device->driver->setstat(code, data, len);
}

// --- THE INTERRUPT KEY ----------------------------------------------------
// Ctrl-C has to be caught where the byte arrives, not where it is read. The
// process it is meant for is usually blocked in a rendezvous and reading
// nothing at all -- wifi scan sits in WAIT_REPLY for eight seconds -- and the
// only thing reading the keyboard at that moment is the other shell.
//
// So it never becomes data while a command is running: the driver hands it here
// instead, and here it ends the process the shell said was in front.

int32_t ubiqos_process_kill(int32_t pid);
bool    ubiqos_intr_request(int32_t pid);

int32_t ubiqos_io_set_foreground(int32_t path, int32_t pid, int32_t owner_pid) {
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    // Cast away const: the device is shared, and this is a property of the
    // device rather than of the path that named it.
    ((ubiqos_device_t*)p->device)->foreground = pid > 0 ? pid : -1;
    return 0;
}

// True when there was something to interrupt. False means the key was not
// consumed and should go through as an ordinary character -- with no command
// running there is a shell reading, and it can do something better with it than
// the kernel can.
bool ubiqos_io_interrupt(const char *name) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (!str_eq(devices[i].name, name)) continue;
        int32_t victim = devices[i].foreground;
        if (victim <= 0) return false;

        // Echoed the way a terminal has always echoed it, and written before
        // interrupts go off: a write to the console can wait for room, and
        // waiting with interrupts off would wait for a thread that cannot run.
        if (devices[i].driver->write)
            devices[i].driver->write((const uint8_t*)"^C\r\n", 4);

        // This runs in the USB thread, not in a trap, so the scheduler's queues
        // are not otherwise ours to touch.
        //
        // A process that asked to hear about the key is told and given a moment
        // to end itself -- a scanner writes Ctrl-C to its dongle and closes it,
        // which killing outright would skip. The foreground stays set in that
        // case so a second press escalates, and the deadline in the scheduler
        // ends it regardless if it does not go.
        uint32_t st = ubiqos_critical_enter();
        bool told = ubiqos_intr_request(victim);
        if (!told) {
            devices[i].foreground = -1;
            ubiqos_process_kill(victim);
        }
        ubiqos_critical_exit(st);
        return true;
    }
    return false;
}

bool ubiqos_io_readable(int32_t path, int32_t owner_pid) {
    return ubiqos_io_readable_count(path, owner_pid) > 0;
}

// How much, rather than whether. A device with no readable entry point is not
// an error: it has nothing waiting, which is what zero says.
int32_t ubiqos_io_readable_count(int32_t path, int32_t owner_pid) {
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    // An exhausted pipe reads as ready, because what it has ready is the end of
    // itself: a reader that stayed blocked would wait for a writer that has
    // gone. The read call sorts the two apart with ubiqos_io_at_eof.
    if (q) return q->pipe_write ? 0
         : (int32_t)pipe_used(&pipes[q->pipe]) + (pipes[q->pipe].writers ? 0 : 1);

    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    if (!p->device->driver->readable) return 0;
    return p->device->driver->readable();
}

bool ubiqos_io_writable(int32_t path, int32_t owner_pid) {
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    // Room, or nobody left to read it -- and the second counts as writable so
    // that a writer into a pipe nobody holds fails rather than waits for ever.
    if (q) return q->pipe_write
        && (pipe_used(&pipes[q->pipe]) < UBIQOS_PIPE_BUF - 1 || !pipes[q->pipe].readers);

    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p) return false;
    if (!p->device->driver->writable) return true;      // cannot fill up
    return p->device->driver->writable() > 0;
}

int32_t ubiqos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid) {
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    if (q) {
        if (q->pipe_write) return -1;                   // the wrong end
        uint32_t st = ubiqos_critical_enter();
        pipe_t *r = &pipes[q->pipe];
        uint32_t n = 0;
        while (n < len && pipe_used(r)) {
            buf[n++] = r->buf[r->tail];
            r->tail = (r->tail + 1) % UBIQOS_PIPE_BUF;
        }
        ubiqos_critical_exit(st);
        return (int32_t)n;
    }
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p || !p->device->driver->read) return -1;
    return p->device->driver->read(buf, len);
}

// Whether any OTHER descriptor of this process still names the same device.
// Two can, since dup, and closing the driver while one of them is still open
// would take the device away from a descriptor that never asked.
static bool device_shared(int32_t path, int32_t owner_pid) {
    const ubiqos_device_t *d = paths[owner_pid][path].device;
    for (int i = 0; i < UBIQOS_MAX_PATHS; i++)
        if (i != path && paths[owner_pid][i].device == d) return true;
    return false;
}

int32_t ubiqos_io_close(int32_t path, int32_t owner_pid) {
    ubiqos_path_t *f = file_entry(path, owner_pid);
    if (f) {
        uint32_t st = ubiqos_critical_enter();
        file_release(f);
        ubiqos_critical_exit(st);
        return 0;
    }
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    if (q) {
        uint32_t st = ubiqos_critical_enter();
        pipe_release(q);
        ubiqos_critical_exit(st);
        return 0;
    }
    ubiqos_path_t *p = path_of(path, owner_pid);
    if (!p) return -1;
    // Guarded, like readable and at_eof beside it. close is optional: a device
    // with nothing to shut down leaves it out, and the first driver that did
    // -- the ADC -- called a null pointer here and took the machine down with
    // it. The failure looked like anything but this, because the console
    // output that would have said where was still in the USB buffer when the
    // fault hit.
    if (!device_shared(path, owner_pid) && p->device->driver->close)
        p->device->driver->close();
    p->device = 0;
    return 0;
}

static ubiqos_path_t *pipe_entry(int32_t path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return 0;
    if (path < 0 || path >= UBIQOS_MAX_PATHS) return 0;
    if (paths[owner_pid][path].pipe < 0) return 0;
    return &paths[owner_pid][path];
}

int32_t ubiqos_io_pipe(int32_t fds[2], int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS || !fds) return -1;
    uint32_t st = ubiqos_critical_enter();

    int32_t q = -1;
    for (int i = 0; i < UBIQOS_MAX_PIPES; i++)
        if (!pipes[i].readers && !pipes[i].writers) { q = i; break; }

    int32_t r = -1, w = -1;
    for (int i = 0; i < UBIQOS_MAX_PATHS; i++) {
        if (paths[owner_pid][i].device || paths[owner_pid][i].file >= 0
            || paths[owner_pid][i].pipe >= 0) continue;
        if (r < 0) r = i; else { w = i; break; }
    }
    if (q < 0 || w < 0) { ubiqos_critical_exit(st); return -1; }

    pipes[q].head = pipes[q].tail = 0;
    pipes[q].readers = pipes[q].writers = 1;
    paths[owner_pid][r].pipe = (int16_t)q; paths[owner_pid][r].pipe_write = 0;
    paths[owner_pid][w].pipe = (int16_t)q; paths[owner_pid][w].pipe_write = 1;
    fds[0] = r;
    fds[1] = w;

    ubiqos_critical_exit(st);
    return 0;
}

bool ubiqos_io_at_eof(int32_t path, int32_t owner_pid) {
    ubiqos_path_t *q = pipe_entry(path, owner_pid);
    if (q) return !q->pipe_write
             && !pipe_used(&pipes[q->pipe]) && !pipes[q->pipe].writers;

    ubiqos_path_t *p = path_of(path, owner_pid);
    return p && p->device->driver->at_eof && p->device->driver->at_eof();
}

// Letting go of one end. A reader blocked on an empty pipe is released by the
// writer's last close, which is why the counts matter more than the buffer.
static void pipe_release(ubiqos_path_t *p) {
    if (p->pipe < 0) return;
    if (p->pipe_write) { if (pipes[p->pipe].writers) pipes[p->pipe].writers--; }
    else               { if (pipes[p->pipe].readers) pipes[p->pipe].readers--; }
    p->pipe = -1;
}

int32_t ubiqos_io_dup(int32_t path, int32_t new_path, int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return -1;
    if (path < 0 || path >= UBIQOS_MAX_PATHS) return -1;
    ubiqos_path_t *src = &paths[owner_pid][path];
    if (!src->device && src->file < 0 && src->pipe < 0) return -1;
    if (new_path == path) return path;                  // dup2 onto itself

    uint32_t st = ubiqos_critical_enter();
    if (new_path < 0) {
        for (int i = 0; i < UBIQOS_MAX_PATHS; i++)
            if (is_free(&paths[owner_pid][i])) {
                new_path = i;
                break;
            }
        if (new_path < 0) { ubiqos_critical_exit(st); return -1; }
    } else if (new_path >= UBIQOS_MAX_PATHS) {
        ubiqos_critical_exit(st);
        return -1;
    } else {
        ubiqos_critical_exit(st);
        ubiqos_io_close(new_path, owner_pid);           // dup2 closes it first
        st = ubiqos_critical_enter();
    }

    paths[owner_pid][new_path] = *src;
    if (src->file >= 0) open_files[src->file].refs++;   // one more descriptor on it
    if (src->pipe >= 0) {
        if (src->pipe_write) pipes[src->pipe].writers++;
        else                 pipes[src->pipe].readers++;
    }
    ubiqos_critical_exit(st);
    return new_path;
}

void ubiqos_io_close_all(int32_t owner_pid) {
    if (owner_pid < 0 || owner_pid >= UBIQOS_MAX_PROCS) return;
    uint32_t st = ubiqos_critical_enter();
    for (int i = 0; i < UBIQOS_MAX_PATHS; i++) {
        if (paths[owner_pid][i].file >= 0) file_release(&paths[owner_pid][i]);
        if (paths[owner_pid][i].pipe >= 0) pipe_release(&paths[owner_pid][i]);
        if (paths[owner_pid][i].device) {
            if (paths[owner_pid][i].device->driver->close)
                paths[owner_pid][i].device->driver->close();
            paths[owner_pid][i].device = 0;
        }
    }
    ubiqos_critical_exit(st);
}
