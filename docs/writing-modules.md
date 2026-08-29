# Writing modules

A myrtos module is loaded at an address nobody knew when it was compiled, and
one copy of it is shared by every process running it. Both facts come from the
same requirement: **the module may not contain absolute addresses**, and it may
not contain writable data.

The build checks this. [`check_module.py`](../check_module.py) reads the
relocations out of the object files and refuses the module if an allocated
section holds an absolute reference. Without that check the fault shows up as a
crash after loading, at an address that means nothing.

This document is about the one constraint that surprises people, because the
source that breaks it looks entirely ordinary.

## The trap: tables of pointers

A table of pointers is a table of addresses, and the linker writes real
addresses into it. Move the module and every entry is wrong.

What makes it hard to spot is that you often did not write a table. The
compiler wrote it for you. All of these produce absolute relocations:

```c
// A switch returning string literals
const char *name(int s) {
    switch (s) { case 0: return "zero"; case 1: return "one"; /* ... */ }
}

// The same thing as an if-chain -- no better
const char *name(int s) {
    if (s == 0) return "zero";
    if (s == 1) return "one";
    /* ... */
}

// A table you did write
static const char *const names[] = { "zero", "one", "two" };

// Function pointers, including every C++ vtable
static void (*const ops[])(void) = { do_read, do_write };
```

Rewriting the switch as an if-chain does **not** help. GCC recognises the shape
either way and builds the same array of pointers.

## What is safe

Measured with the flags modules are actually built with
(`-fno-pic -mcmodel=medany -ffreestanding -nostdlib -O2`), counting absolute
`R_RISCV_32` relocations:

| Construct | Absolute relocations |
|---|---|
| `switch` returning string literals | 6 |
| if-chain returning string literals | 6 |
| array of string pointers | 6 |
| array of function pointers | 2 |
| `switch` returning integers | 0 |
| `switch` with different bodies (a jump table) | 0 |
| two-dimensional `char` array | 0 |
| one string, walked by index | 0 |

A `switch` is not the problem. A `switch` over *code* compiles to a jump table
of label differences -- `R_RISCV_ADD32` and `R_RISCV_SUB32`, which are
link-time constants carrying no absolute address, and are perfectly fine. It is
a `switch` over *pointers* that breaks, and it breaks for the same reason a
hand-written pointer array does.

## Fixing it

For strings, give the table no pointers to hold. A two-dimensional array stores
the characters themselves:

```c
static const char names[6][6] = { "zero", "one", "two", "three", "four", "five" };
const char *name(int s) { return (s >= 0 && s < 6) ? names[s] : "?"; }
```

That costs the width of the longest entry for every row, and it is the simplest
thing that works. Where the entries vary too much in length to pad, put them end
to end in one string and count NULs -- `modules/ps/ps.c` does this.

For function tables that genuinely need a runtime choice, store the *distance*
from the table to each function rather than its address, and add the table's own
address when calling. The `MYRTOS_RELTAB_*` macros in
[`common/myrtos_abi.h`](../common/myrtos_abi.h) do this; the differences are
computed by the assembler, because C rejects them as initialisers.

## It depends on the optimisation level

The same source, compiled at different levels:

| | `-O0` | `-O1` | `-O2` | `-Os` | `-O3` |
|---|---|---|---|---|---|
| `switch` returning literals | 0 | 0 | 6 | 6 | 6 |
| if-chain returning literals | 0 | 0 | 6 | 6 | 6 |

At `-O0` and `-O1` the compiler emits the comparisons and loads each literal
where it is used. From `-O2` it decides a table is cheaper.

So a module can pass at one optimisation level and fail at another without a
line of source changing. This is why the check reads the real object files from
the real build rather than analysing the source: the answer depends on what the
compiler actually did.

## Diagnosing it yourself

When the build refuses a module, it names the section and the symbol. To look
directly:

```bash
riscv32-unknown-elf-readelf -rW \
    build/CMakeFiles/NAME_app.dir/modules/NAME/NAME.c.o
```

Absolute entries appear as `R_RISCV_32` against a `.LC` label (a string
literal) or a function name. `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`,
`R_RISCV_CALL_PLT`, `R_RISCV_BRANCH`, `R_RISCV_ADD32` and `R_RISCV_SUB32` are
all position independent and are what a clean module contains.

Note the `-W`: without it `readelf` truncates the type names, and every
relocation looks like something the checker does not recognise.

## Memory, and where the pointer lives

A module gets one block from the kernel: `mem_size` in its header, holding its
data and its stack. `myrtos_alloc`, `myrtos_free` and `myrtos_realloc` ask for
more. The kernel records which process each block belongs to, so a process that
dies -- including one that dies without tidying up -- returns everything.

The two rules meet here. A module may not have writable statics, so this is
refused by the build:

```c
static void *buffer;            // .sbss -- rejected
void module_main(void) { buffer = myrtos_alloc(1000); }
```

Keep the pointer on the stack, or in the data area the header reserved:

```c
void module_main(void) {
    void *buffer = myrtos_alloc(1000);
    /* ... */
    myrtos_free(buffer);
}
```

`myrtos_free` refuses a pointer this process was not given, so one module cannot
release another's memory, or the kernel's.

## The other rule

A module may not have writable data. `.data`, `.bss`, `.sdata` and `.sbss` must
all be empty, because two processes sharing the code would write to the same
variables. Anything a process needs to keep goes in its own memory area, which
the module header asks for with `mem_size`.

A `static int counter;` in a module is refused by the same check. Note which
section it lands in: small objects go to `.sbss` rather than `.bss` on this
target, which is why the check looks at all four.

    MODULE IS NOT POSITION INDEPENDENT:
      badtest_app.elf: writable section .sbss is present
