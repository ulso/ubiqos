#ifndef MYRTOS_TLSF_H
#define MYRTOS_TLSF_H

#include <stddef.h>
#include <stdint.h>

// A classic 32-bit TLSF structure adapted for embedded systems.
// SL_INDEX_COUNT_LOG2 = 5 means each first-level block (a power of two) is
// split into 32 smaller, linear sub-blocks to minimise fragmentation.
#define FL_INDEX_MAX    32
#define SL_INDEX_COUNT  32

typedef void* tlsf_pool_t;

// Function declarations for the myrtos memory pool
tlsf_pool_t myrtos_tlsf_create(void* mem, size_t bytes);
void* myrtos_tlsf_malloc(tlsf_pool_t pool, size_t size);
void myrtos_tlsf_free(tlsf_pool_t pool, void* ptr);
size_t myrtos_tlsf_largest_free(tlsf_pool_t pool);

#endif // MYRTOS_TLSF_H
