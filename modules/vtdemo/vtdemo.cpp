#include "../../common/myrtos_abi.h"

// A real virtual call: the vtable is a table of function pointers, which is the
// one thing a module could not contain before the loader could relocate.
struct Shape {
    virtual const char *name() const = 0;
    virtual uint32_t sides() const = 0;
};
struct Circle : Shape {
    const char *name() const override { return "circle\n"; }
    uint32_t sides() const override { return 0; }
};
struct Square : Shape {
    const char *name() const override { return "square\n"; }
    uint32_t sides() const override { return 4; }
};

// Not inlined, so the compiler cannot see which type comes back and has to go
// through the vtable. Both objects live on the caller's stack: a static one
// would put its vptr in .data, and a shareable module may not have that.
__attribute__((noinline))
static Shape *pick(int argc, Circle *c, Square *s) {
    return (argc > 1) ? (Shape *)s : (Shape *)c;
}

extern "C" void module_main(int argc, char **argv) {
    (void)argv;
    Circle c; Square s;
    Shape *p = pick(argc, &c, &s);

    myrtos_write_str(MYRTOS_STDOUT, p->name());
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "sides ");
    myrtos_line_u32(&l, p->sides());
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
