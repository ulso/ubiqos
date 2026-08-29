#ifndef MYRTOS_IO_H
#define MYRTOS_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"

// The I/O manager, in the spirit of OS-9: processes open a path to a named
// device and write to the path number. The driver owns the hardware.
//
// Devices come from DESCRIPTORS, not from a table in the code. A descriptor is
// a data module on the card stating what the device is called, which driver
// handles it, and carrying a configuration only the driver interprets. Adding a
// device is then a matter of adding a file.

#define MYRTOS_MAX_DRIVERS 4
#define MYRTOS_MAX_DEVICES 4
#define MYRTOS_MAX_PATHS   8
#define MYRTOS_PATH_NONE   (-1)

typedef struct {
    const char *module_name;    // what the descriptor refers to: "UART    MOD"
    int32_t (*configure)(const void *config, uint32_t size);
    int32_t (*open)(void);
    int32_t (*write)(const uint8_t *buf, uint32_t len);
    int32_t (*read)(uint8_t *buf, uint32_t len);   // 0 = nothing right now
    // Whether a read would return anything. A driver without this is never
    // waited on: reads from it keep returning 0, as they did before blocking
    // existed. That is what keeps the send-only UART from parking a shell
    // forever on input that cannot arrive.
    int32_t (*readable)(void);
    int32_t (*close)(void);
} myrtos_driver_t;

void    myrtos_io_init(void);
uint32_t myrtos_io_device_count(void);

// Register a device from a descriptor. The driver is looked up by name and is
// handed the configuration tail; an unrecognised name rejects the descriptor.
bool    myrtos_io_add_descriptor(const myrtos_descriptor_t *desc);

int32_t myrtos_io_open(const char *name, int32_t owner_pid);
int32_t myrtos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_close(int32_t path, int32_t owner_pid);
void    myrtos_io_close_all(int32_t owner_pid);
void    myrtos_io_inherit(int32_t parent_pid, int32_t child_pid);

// Whether a read on this path would return something. False also for a device
// whose driver cannot answer, so that such a path is never blocked on.
bool     myrtos_io_readable(int32_t path, int32_t owner_pid);

// Open on a SPECIFIC path number. The kernel uses it to give the first process
// its 0, 1 and 2; ordinary opens take the first free slot.
int32_t myrtos_io_open_as(const char *name, int32_t owner_pid, int32_t path);

#endif
