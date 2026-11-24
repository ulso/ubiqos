#ifndef BLIXT_MODWRITE_H
#define BLIXT_MODWRITE_H

#include <stdint.h>
#include "asm.h"

// The module as bytes, ready for a file; the caller frees it. 0 when there is
// no memory.
uint8_t *mod_build(const char *name, const asm_result_t *r, uint32_t *size_out);

#endif
