#include <stdint.h>
extern "C" {
#include "../../common/myrtos_abi.h"
}

// A class with no virtual functions: member functions are ordinary functions
// with a hidden this argument, and are inlined here besides. No vtable, no
// static constructors.
class Terminal {
public:
    explicit Terminal(const char *device) : path_(myrtos_open(device)) {}
    bool ok() const { return path_ >= 0; }
    void write(const char *s) const { myrtos_write_str(path_, s); }
    void close() const { myrtos_close(path_); }
private:
    int32_t path_;
};

// extern "C" is required: the build looks module_main up with nm, and C++ would
// otherwise mangle the name to _Z11module_mainv.
// Static polymorphism with CRTP: the base class knows the derived type through
// the template parameter, so the call binds at compile time. No vtable, hence no
// function pointers in static data.
template <typename Derived>
struct Writer {
    void emit(const Terminal &t) const {
        static_cast<const Derived*>(this)->emit_impl(t);
    }
};

struct Plain : Writer<Plain> {
    void emit_impl(const Terminal &t) const { t.write("[plain] static dispatch\n"); }
};

struct Loud : Writer<Loud> {
    void emit_impl(const Terminal &t) const { t.write("[LOUD] STATIC DISPATCH\n"); }
};

// If a runtime choice is needed anyway: build the table in the process's own
// memory rather than as a static initialiser. The addresses are then computed at
// runtime and travel with the module, instead of the linker writing them in.
using Emitter = void (*)(const Terminal &);

extern "C" {
// used: the functions are referenced only from assembly, which the compiler
// cannot see, and would otherwise be optimised away as unused.
__attribute__((used)) static const char *say_first(void)  { return "[reltab 0] shared, in .rodata\n"; }
__attribute__((used)) static const char *say_second(void) { return "[reltab 1] shared, in .rodata\n"; }
}
MYRTOS_RELTAB_BEGIN(emitters);
MYRTOS_RELTAB_ENTRY(emitters, say_first);
MYRTOS_RELTAB_ENTRY(emitters, say_second);
MYRTOS_RELTAB_END();

extern "C" void module_main(void) {
    Terminal term("term");
    if (!term.ok()) { myrtos_exit(); return; }

    term.write("\n[c++] classes, templates and RAII, no vtables\n");

    Plain plain; Loud loud;
    plain.emit(term);
    loud.emit(term);

    // The shared variant: the table sits const in .rodata and carries offsets
    // rather than addresses, so every process uses the same copy.
    term.write(MYRTOS_RELTAB_CALL(emitters, 1, const char *(*)(void))());

    // And the stack-built one, which works but gives each process its own copy.
    Emitter table[2] = {
        [](const Terminal &t) { t.write("[table 0] built at runtime\n"); },
        [](const Terminal &t) { t.write("[table 1] built at runtime\n"); },
    };
    table[term.ok() ? 1 : 0](term);
    for (int i = 0; i < 3; i++) {
        term.write("[c++] still position independent\n");
    }
    term.close();
}
