#include "../../common/ubiqos_abi.h"

// Device descriptor for the machine's own console: the display for output, the
// USB keyboard for input. No configuration tail -- there is one screen and one
// host controller, and both belong to drivers that know where they are.
//
// With this registered a shell can be given "con" for its standard paths and
// runs with no host computer attached at all. The serial shell keeps working
// alongside it; they are separate processes with separate paths.

typedef struct {
    ubiqos_descriptor_t desc;
} con_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const con_descriptor_t con_descriptor = {
    .desc = {
        .device_name   = "con",
        .driver_name   = "console",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
