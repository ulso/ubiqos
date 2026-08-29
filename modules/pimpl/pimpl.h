#pragma once
#include "../../common/myrtos_module.h"

// Everything the module remembers lives here. MYRTOS_MODULE declares one
// instance as __thread, so the linker puts it in the module's thread-local
// block and every process gets its own zeroed copy. Split across files, each of
// them reaches it through `this`, which costs one instruction from tp.
struct Pimpl : MyrtosModule<Pimpl> {
    uint32_t counter;
    char     label[16];

    void bump(uint32_t times);            // defined in bump.cpp
    void run(int argc, char **argv);      // defined in pimpl.cpp
};
