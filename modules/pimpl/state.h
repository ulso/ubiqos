#pragma once
#include "../../common/myrtos_abi.h"

// The module's state, in the shape of a pimpl. The layout is visible only to
// files that include this header, and the storage is the process's own data
// area rather than a static -- one copy of the code is shared by every process
// running it, so a static would be shared too.
//
// Where a pimpl reaches its Impl through `this`, this reaches the data area
// through the process. Nothing has to be passed from one function to the next.
typedef struct {
    uint32_t counter;
    char     label[16];
} state_t;

static inline state_t *state(void) {
    return (state_t *)myrtos_data_area(0);
}
