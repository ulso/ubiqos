#include "state.h"

// A second source file, reaching the same state without being handed anything.
//
// state() is a system call, so it is fetched once and kept. Written inside the
// loop it compiled to an ecall per increment -- a full trap into the kernel and
// back to add one to an integer. This is where OS-9 was better than we are: with
// the data area in a register, the compiler would have emitted a single
// load-add-store and there would be nothing to remember.
void bump(uint32_t times) {
    state_t *s = state();
    for (uint32_t i = 0; i < times; i++) s->counter++;
}
