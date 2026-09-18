#include "tlsf.h"
#include "critical.h"

// Block header. size is the payload size; bit 0 marks it free.
// prev_phys_block points at the neighbour at the next lower address, which is
// what makes coalescing backwards possible.
typedef struct block_header_t {
    struct block_header_t* prev_phys_block;
    size_t size;
    struct block_header_t* next_free;
    struct block_header_t* prev_free;
} block_header_t;

typedef struct {
    block_header_t block_null;
    uint32_t fl_bitmap;
    uint32_t sl_bitmap[FL_INDEX_MAX];
    block_header_t* blocks[FL_INDEX_MAX][SL_INDEX_COUNT];
    uintptr_t pool_start;
    uintptr_t pool_end;
} tlsf_ctrl_t;

#define MIN_PAYLOAD      (2 * sizeof(void*))
#define BLOCK_FREE_BIT   1U
#define BLOCK_SIZE(b)    ((b)->size & ~(size_t)BLOCK_FREE_BIT)
#define BLOCK_IS_FREE(b) (((b)->size & BLOCK_FREE_BIT) != 0)

static inline int tlsf_fls(uint32_t word) {
    if (!word) return -1;
    return 31 - __builtin_clz(word);
}

static void tlsf_mapping(size_t size, int* fl, int* sl) {
    int f = tlsf_fls((uint32_t)size);
    if (f < 5) {
        *fl = 0;
        *sl = (int)(size & (SL_INDEX_COUNT - 1));
    } else {
        *fl = f;
        *sl = (int)((size >> (f - 5)) & (SL_INDEX_COUNT - 1));
    }
    if (*fl >= FL_INDEX_MAX) *fl = FL_INDEX_MAX - 1;
}

// The neighbour at the next higher address, or NULL if the block is last.
static block_header_t* next_phys(tlsf_ctrl_t* ctrl, block_header_t* block) {
    uintptr_t next = (uintptr_t)block + sizeof(block_header_t) + BLOCK_SIZE(block);
    if (next >= ctrl->pool_end) return 0;
    return (block_header_t*)next;
}

static void tlsf_insert(tlsf_ctrl_t* ctrl, block_header_t* block) {
    int fl, sl;
    tlsf_mapping(BLOCK_SIZE(block), &fl, &sl);

    block->size |= BLOCK_FREE_BIT;
    block->next_free = ctrl->blocks[fl][sl];
    block->prev_free = &ctrl->block_null;
    if (ctrl->blocks[fl][sl] != &ctrl->block_null) {
        ctrl->blocks[fl][sl]->prev_free = block;
    }
    ctrl->blocks[fl][sl] = block;
    ctrl->fl_bitmap |= (1U << fl);
    ctrl->sl_bitmap[fl] |= (1U << sl);
}

// Pull a specific block out of its free list, wherever in it it sits.
// Coalescing needs exactly that: the neighbour to be eaten is rarely first.
static void tlsf_remove(tlsf_ctrl_t* ctrl, block_header_t* block) {
    int fl, sl;
    tlsf_mapping(BLOCK_SIZE(block), &fl, &sl);

    if (block->prev_free != &ctrl->block_null) {
        block->prev_free->next_free = block->next_free;
    } else if (ctrl->blocks[fl][sl] == block) {
        ctrl->blocks[fl][sl] = block->next_free;
        if (ctrl->blocks[fl][sl] == &ctrl->block_null) {
            ctrl->sl_bitmap[fl] &= ~(1U << sl);
            if (!ctrl->sl_bitmap[fl]) ctrl->fl_bitmap &= ~(1U << fl);
        }
    }
    if (block->next_free != &ctrl->block_null) {
        block->next_free->prev_free = block->prev_free;
    }
    block->size &= ~(size_t)BLOCK_FREE_BIT;
}

tlsf_pool_t ubiqos_tlsf_create(void* mem, size_t bytes) {
    if (bytes < sizeof(tlsf_ctrl_t) + sizeof(block_header_t) + MIN_PAYLOAD) return NULL;

    tlsf_ctrl_t* ctrl = (tlsf_ctrl_t*)mem;
    ctrl->fl_bitmap = 0;
    ctrl->block_null.next_free = &ctrl->block_null;
    ctrl->block_null.prev_free = &ctrl->block_null;
    ctrl->block_null.size = 0;
    for (int i = 0; i < FL_INDEX_MAX; ++i) {
        ctrl->sl_bitmap[i] = 0;
        for (int j = 0; j < SL_INDEX_COUNT; ++j) ctrl->blocks[i][j] = &ctrl->block_null;
    }

    uintptr_t start = ((uintptr_t)mem + sizeof(tlsf_ctrl_t) + 3) & ~(uintptr_t)3;
    ctrl->pool_start = start;
    ctrl->pool_end = (uintptr_t)mem + bytes;

    block_header_t* first = (block_header_t*)start;
    first->prev_phys_block = 0;
    first->size = (ctrl->pool_end - start) - sizeof(block_header_t);
    tlsf_insert(ctrl, first);
    return (tlsf_pool_t)ctrl;
}

static void* ubiqos_tlsf_malloc_unlocked(tlsf_pool_t pool, size_t size) {
    tlsf_ctrl_t* ctrl = (tlsf_ctrl_t*)pool;
    if (!ctrl || !size) return NULL;

    size = (size + 3) & ~(size_t)3;
    if (size < MIN_PAYLOAD) size = MIN_PAYLOAD;

    int fl, sl;
    tlsf_mapping(size, &fl, &sl);

    block_header_t* block = 0;
    for (int f = fl; f < FL_INDEX_MAX && !block; ++f) {
        uint32_t sl_map = ctrl->sl_bitmap[f];
        if (f == fl) sl_map &= (sl >= 31) ? 0u : (~0U << (sl + 1));
        while (sl_map) {
            int s = __builtin_ctz(sl_map);
            block_header_t* cand = ctrl->blocks[f][s];
            if (cand != &ctrl->block_null && BLOCK_SIZE(cand) >= size) {
                tlsf_remove(ctrl, cand);
                block = cand;
                break;
            }
            sl_map &= ~(1U << s);
        }
    }
    if (!block) return NULL;

    size_t remain = BLOCK_SIZE(block) - size;
    if (remain >= sizeof(block_header_t) + MIN_PAYLOAD) {
        block_header_t* rest =
            (block_header_t*)((uintptr_t)block + sizeof(block_header_t) + size);
        rest->prev_phys_block = block;
        rest->size = remain - sizeof(block_header_t);
        block->size = size;
        block_header_t* after = next_phys(ctrl, rest);
        if (after) after->prev_phys_block = rest;
        tlsf_insert(ctrl, rest);
    }

    block->size &= ~(size_t)BLOCK_FREE_BIT;
    return (void*)((uintptr_t)block + sizeof(block_header_t));
}

static void ubiqos_tlsf_free_unlocked(tlsf_pool_t pool, void* ptr) {
    tlsf_ctrl_t* ctrl = (tlsf_ctrl_t*)pool;
    if (!ctrl || !ptr) return;

    block_header_t* block =
        (block_header_t*)((uintptr_t)ptr - sizeof(block_header_t));

// Coalesce forwards: the neighbour at the next higher address is pulled from
// its list and its space, its header included, becomes part of this block.
    block_header_t* next = next_phys(ctrl, block);
    if (next && BLOCK_IS_FREE(next)) {
        tlsf_remove(ctrl, next);
        block->size = BLOCK_SIZE(block) + sizeof(block_header_t) + BLOCK_SIZE(next);
    }

// Coalesce backwards: then it is the neighbour that grows, and the block goes.
    block_header_t* prev = block->prev_phys_block;
    if (prev && BLOCK_IS_FREE(prev)) {
        tlsf_remove(ctrl, prev);
        prev->size = BLOCK_SIZE(prev) + sizeof(block_header_t) + BLOCK_SIZE(block);
        block = prev;
    }

    block_header_t* after = next_phys(ctrl, block);
    if (after) after->prev_phys_block = block;

    tlsf_insert(ctrl, block);
}

// The largest contiguous free block. It exists to show that coalescing really
// happens: without it this number shrinks with every cycle of allocation and
// freeing.
static size_t ubiqos_tlsf_largest_free_unlocked(tlsf_pool_t pool) {
    tlsf_ctrl_t* ctrl = (tlsf_ctrl_t*)pool;
    size_t best = 0;
    for (uintptr_t p = ctrl->pool_start; p < ctrl->pool_end; ) {
        block_header_t* b = (block_header_t*)p;
        if (BLOCK_IS_FREE(b) && BLOCK_SIZE(b) > best) best = BLOCK_SIZE(b);
        p += sizeof(block_header_t) + BLOCK_SIZE(b);
        if (BLOCK_SIZE(b) == 0) break;
    }
    return best;
}


bool ubiqos_tlsf_owns(tlsf_pool_t pool, const void* p) {
    tlsf_ctrl_t* ctrl = (tlsf_ctrl_t*)pool;
    if (!ctrl || !p) return false;
    return (uintptr_t)p >= ctrl->pool_start && (uintptr_t)p < ctrl->pool_end;
}

// --- THE LOCK -------------------------------------------------------------
// The free lists are walked and rewritten by two kinds of caller, and until now
// nothing kept them apart.
//
// A system call runs in a trap with interrupts off, so SYS_ALLOC, SYS_FREE,
// SYS_EXEC and a process's own teardown were always atomic. A kernel thread is
// not: the filesystem server allocates its staging buffer and the wifi server
// its scan table with interrupts on, and either can be preempted anywhere --
// including between taking a block out of a free list and putting the remainder
// back. A system call arriving in that window works on a list that is halfway
// through being changed, and hands out a block that is still on it.
//
// That is a race with no symptom of its own. It shows up later as a block
// delivered twice, or a link into nothing, at whatever moment the damage is
// finally read -- which is exactly the shape of a fault that is there one boot
// and gone the next.
//
// A critical section rather than a server process, and the difference is worth
// stating: the filesystem became a process because its work is long, measured
// in milliseconds, and holding the machine for that is what broke the keyboard.
// TLSF is O(1) and takes microseconds, so there is nothing to hold. A server
// would also cost two context switches on the path taken by every exec, and
// could not create a process without one -- process creation allocates.
//
// Nesting is free: save_and_disable_interrupts returns the previous state and
// restore_interrupts puts that state back, so a call from a trap leaves
// interrupts off, as they already were.
void* ubiqos_tlsf_malloc(tlsf_pool_t pool, size_t size) {
    uint32_t st = ubiqos_critical_enter();
    void* p = ubiqos_tlsf_malloc_unlocked(pool, size);
    ubiqos_critical_exit(st);
    return p;
}

void ubiqos_tlsf_free(tlsf_pool_t pool, void* ptr) {
    uint32_t st = ubiqos_critical_enter();
    ubiqos_tlsf_free_unlocked(pool, ptr);
    ubiqos_critical_exit(st);
}

// Walking the pool block by block reads the same structure the other two write,
// so it needs the same protection -- a torn walk reports a number that was
// never true, and free is the one number a caller acts on.
size_t ubiqos_tlsf_largest_free(tlsf_pool_t pool) {
    uint32_t st = ubiqos_critical_enter();
    size_t n = ubiqos_tlsf_largest_free_unlocked(pool);
    ubiqos_critical_exit(st);
    return n;
}
