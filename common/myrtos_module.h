#pragma once
#include "myrtos_abi.h"

// The C++ face of a module.
//
// A module may not keep state in a file-scope variable: one copy of the code is
// shared by every process running it, so the variable would be shared too. C++
// solves this without being asked. Member variables are addressed through
// `this`, which is a runtime pointer -- exactly the base-relative addressing
// OS-9 got from the U register, except the compiler emits it for you.
//
// Derive from this, put every variable in the class, and the problem is gone.
//
//     struct Counter : MyrtosModule<Counter> {
//         unsigned count;
//         void run(int argc, char **argv) { count++; }
//     };
//     MYRTOS_MODULE(Counter)
//
// Two rules remain, and they are the ones C++ can break by itself:
//
//   No virtual functions. A vtable is a table of function pointers, and those
//   are absolute addresses. Use CRTP where you want an interface -- the base
//   knows the derived type through the template parameter, so the call binds at
//   compile time and no vtable is emitted. modules/cxxdemo shows it.
//
//   No global instances. A constructor at file scope puts a pointer in
//   .init_array, which is the same kind of table.

template <class Derived>
struct MyrtosModule {
    // The process's own data area, cast to the derived type. Fetch it once and
    // keep it: this is a system call, so calling it in a loop costs a trap each
    // time and stops the compiler optimising across it.
    static Derived *state() {
        return static_cast<Derived *>(myrtos_data_area(0));
    }

    // How much room the data area has. The stack grows down into the same span,
    // so this is what exists rather than what is safe to fill.
    static uint32_t room() {
        uint32_t n = 0;
        (void)myrtos_data_area(&n);
        return n;
    }
};

// Generates the entry point the loader looks for. The class must fit the data
// area and must not need a constructor to have run -- the area arrives zeroed.
#define MYRTOS_MODULE(Class)                                        \
    extern "C" void module_main(int argc, char **argv) {            \
        if (Class::room() < sizeof(Class)) {                        \
            myrtos_write_str(MYRTOS_STDERR, "module: data area too small\n"); \
            return;                                                 \
        }                                                           \
        Class::state()->run(argc, argv);                            \
    }
