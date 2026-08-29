#pragma once
#include "../../common/myrtos_module.h"

// Everything the module remembers lives here, and nowhere else. Split across
// files, every one of them reaches it the same way: through `this`.
struct Pimpl : MyrtosModule<Pimpl> {
    uint32_t counter;
    char     label[16];

    void bump(uint32_t times);            // defined in bump.cpp
    void run(int argc, char **argv);      // defined in pimpl.cpp
};
