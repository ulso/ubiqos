#ifndef MYRTOS_TLSF_H
#define MYRTOS_TLSF_H

#include <stddef.h>
#include <stdint.h>

// Vi använder en klassisk 32-bitars TLSF-struktur anpassad för inbäddade system.
// SL_INDEX_COUNT_LOG2 = 5 betyder att varje förstagradsblock (Power of 2) 
// delas upp i 32 stycken mindre, linjära underblock för minimal fragmentering.
#define FL_INDEX_MAX    32
#define SL_INDEX_COUNT  32

typedef void* tlsf_pool_t;

// Funktionsdeklarationer för Myrtos minnespool
tlsf_pool_t myrtos_tlsf_create(void* mem, size_t bytes);
void* myrtos_tlsf_malloc(tlsf_pool_t pool, size_t size);
void myrtos_tlsf_free(tlsf_pool_t pool, void* ptr);
size_t myrtos_tlsf_largest_free(tlsf_pool_t pool);

#endif // MYRTOS_TLSF_H
