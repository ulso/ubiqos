#include "pimpl.h"

// A second source file. It was handed nothing and reaches no global: `counter`
// is a member, so the compiler addresses it relative to `this`. That is the
// whole trick, and it costs one instruction.
void Pimpl::bump(uint32_t times) {
    for (uint32_t i = 0; i < times; i++) counter++;
}
