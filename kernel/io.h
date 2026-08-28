#ifndef MYRTOS_IO_H
#define MYRTOS_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"

// I/O-hanteraren, i OS-9:s anda: processer öppnar en väg till en namngiven
// enhet och skriver till vägnumret. Drivrutinen äger hårdvaran.
//
// Enheterna kommer från BESKRIVARE, inte från en tabell i koden. En beskrivare
// är en datamodul på kortet som säger vad enheten heter, vilken drivrutin som
// hanterar den, och bär en konfiguration som bara drivrutinen tolkar. Att lägga
// till en enhet är då att lägga till en fil.

#define MYRTOS_MAX_DRIVERS 4
#define MYRTOS_MAX_DEVICES 4
#define MYRTOS_MAX_PATHS   8
#define MYRTOS_PATH_NONE   (-1)

typedef struct {
    const char *module_name;    // som beskrivaren refererar till: "UART    MOD"
    int32_t (*configure)(const void *config, uint32_t size);
    int32_t (*open)(void);
    int32_t (*write)(const uint8_t *buf, uint32_t len);
    int32_t (*read)(uint8_t *buf, uint32_t len);   // 0 = inget just nu
    int32_t (*close)(void);
} myrtos_driver_t;

void    myrtos_io_init(void);
uint32_t myrtos_io_device_count(void);

// Registrera en enhet ur en beskrivare. Drivrutinen slås upp på namn och får
// konfigurationssvansen; känns namnet inte igen avvisas beskrivaren.
bool    myrtos_io_add_descriptor(const myrtos_descriptor_t *desc);

int32_t myrtos_io_open(const char *name, int32_t owner_pid);
int32_t myrtos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_close(int32_t path, int32_t owner_pid);
void    myrtos_io_close_all(int32_t owner_pid);
void    myrtos_io_inherit(int32_t parent_pid, int32_t child_pid);

// Öppna på ett BESTÄMT vägnummer. Kärnan använder den för att ge den första
// processen sina 0, 1 och 2; vanliga öppningar tar första lediga plats.
int32_t myrtos_io_open_as(const char *name, int32_t owner_pid, int32_t path);

#endif
