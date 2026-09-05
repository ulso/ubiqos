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

// Static objects, which is the ordinary way to write this and was refused until
// now: each holds an absolute pointer to its own vtable, so its vptr lands in
// .data -- and a module with writable data could not exist when one copy in
// flash served every process. It gets a copy of its own now, so the data is
// per process and the vptr is one more address the loader fixes.
static Circle circle;
static Square square;

// A counter as well, to say plainly that a static variable is now a variable:
// it is written, and each process starts from zero because each has its own.
static uint32_t calls;

__attribute__((noinline))
static Shape *pick(int argc) {
    return (argc > 1) ? (Shape *)&square : (Shape *)&circle;
}

extern "C" void module_main(int argc, char **argv) {
    (void)argv;
    calls++;
    Shape *p = pick(argc);

    myrtos_write_str(MYRTOS_STDOUT, p->name());
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "sides ");
    myrtos_line_u32(&l, p->sides());
    myrtos_line_str(&l, ", calls ");
    myrtos_line_u32(&l, calls);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
