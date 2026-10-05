// See ../README.md. The chip's own id: 96 bits on an STM32, where the RP2350's
// flash gives 64 -- and the key store takes the size from here.
#pragma once
#include <stdint.h>
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 12
typedef struct { uint8_t id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES]; } pico_unique_board_id_t;
void stm32_unique_id(uint8_t out[12]);
static inline void pico_get_unique_board_id(pico_unique_board_id_t *out) { stm32_unique_id(out->id); }
