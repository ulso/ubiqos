#include "../../common/ubiqos_abi.h"

// Device descriptor for the analogue inputs. No configuration tail: the four
// pins and the sample rate are fixed by the board and by what the interrupt
// experiment wants, and a tail carrying either would only repeat what the
// driver already knows.
typedef struct {
    ubiqos_descriptor_t desc;
} adc_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const adc_descriptor_t adc_descriptor = {
    .desc = {
        .device_name   = "adc",
        .driver_name   = "adcdev",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
