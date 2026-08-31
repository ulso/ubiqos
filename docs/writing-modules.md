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

## Several source files, and where state goes

A module can be split across files -- pass the extra sources to
`myrtos_add_module` after the first. Add `__thread` to any variable that has to
be per process:

```c
static int counter;             // rejected: one copy, shared by every process
static __thread int counter;    // one per process, and it works across files
```

`static` does two things: it hides the name and it gives static storage. It is
the storage that is refused, so making the variable global does not help --
`int counter;` lands in `.bss` just the same, and is exported besides. One copy
of the code is shared by every process running it, so the variable would be
shared too.

`__thread` changes where it lives, not how it is written. The linker gathers
the thread-local variables from every source file into one block and gives each
a fixed offset from `tp`, which the kernel points at the process's own area.
Reaching one is a single instruction:

```
lw   a4,0(tp)
sw   a4,0(tp)
```

That is what OS-9's linker did with a module's data section, and what its U
register held. The module header carries the block's size and where its initial
values sit; the kernel copies those in and zeroes the rest for every process.

`static const` needs none of this. It goes to `.rodata`, is shared deliberately,
and still hides the name.

Where the state is not a simple variable but a block of bytes you want to lay
out yourself, `myrtos_data_area` hands out what is left after the thread-local
block. That is a system call, so fetch it once. This is a pimpl, and it behaves
like one:

```c
// state.h -- private to the module, not exported
typedef struct { uint32_t counter; char label[16]; } state_t;
static inline state_t *state(void) { return (state_t *)myrtos_data_area(0); }
```

```c
// counter.c -- another file, handed nothing, reaching the same state
#include "state.h"
void bump(uint32_t times) { for (uint32_t i = 0; i < times; i++) state()->counter++; }
```

Where a pimpl reaches its `Impl` through `this`, this reaches the data area
through the process. Nothing has to be threaded from one function to the next,
which is the whole ergonomic point; without it you are left passing a handle
into every call.

`modules/pimpl/` is this, built from two files. Two instances running at once
keep separate counters.

Fetch the pointer once and keep it. `myrtos_data_area` is a system call, so
`state()->counter++` inside a loop compiles to an `ecall` per iteration -- a
full trap into the kernel and back to add one to an integer:

```
103d2:  li    a7,24
103da:  ecall              <-- every time round
103de:  lw    a5,0(a0)
103e4:  sw    a5,0(a0)
```

A local `state_t *s = state();` outside the loop reduces that to one call --
and to rather more than that. The compiler cannot know two calls return the
same pointer, so the system call is also an optimisation barrier. With it
hoisted, this loop disappears entirely:

```
103d6:  ecall              <-- once
103dc:  lw    a5,0(a0)
103de:  add   a5,a5,a4     <-- the whole loop, recognised
103e0:  sw    a5,0(a0)
```

### Why this is manual

A normal program addresses its globals PC-relative, because the linker puts
`.text` and `.bss` in one image at a fixed distance apart. The compiler bakes
that distance in.

myrtos breaks exactly that assumption: the code may be in flash and the data
area on the heap, at a distance decided at runtime and different for every
process. So the distance the compiler computed points into the module image,
not into anyone's data.

Which is also why read-only data needs none of this. Strings and `const` tables
*are* in the module image, at a fixed distance from the code, shared on purpose.
PC-relative is exactly right for them. It is only writable per-process state
that has to go through a base obtained at runtime.

OS-9 made this automatic: the data area lived in the U register and the
compiler addressed globals relative to it, so `static int counter;` simply
worked and was private per process. RISC-V has `gp` for the same purpose, and
the trap frame already saves and restores it per process. Making modules use it
would remove both the system call and the struct.

The data area sits inside the block `mem_size` asked for, after the command
line and its argv vector. `myrtos_data_area(&size)` reports what is there --
but the stack grows down into the same span, so that is what exists, not what
is safe. A module that wants a lot should ask for a larger `mem_size` rather
than assume.

## In C++, the problem mostly goes away

A member variable is addressed through `this`, which is a runtime pointer. That
is the base-relative addressing the data area needs, and the compiler emits it
without being asked -- it is OS-9's U register, arrived at from a different
direction. Put every variable in a class, derive from `MyrtosModule`, and there
is nothing left to remember:

```cpp
struct Pimpl : MyrtosModule<Pimpl> {
    uint32_t counter;
    void bump(uint32_t times);          // another file
    void run(int argc, char **argv);
};
MYRTOS_MODULE(Pimpl)
```

`bump` in the second file compiles to this, with `this` in `a0`:

```
lw   a5,0(a0)
add  a5,a5,a1
sw   a5,0(a0)
ret
```

One load and one store against a runtime pointer, and `this` costs nothing to
obtain: the kernel leaves the data area in `tp` when it starts the process, so
the entry point is

```
mv   a0,tp
j    Pimpl::run
```

That is what the thread pointer is for. A process's data area *is* thread-local
storage -- laid out by us rather than by the compiler, which is why `.tdata` and
`.tbss` are empty and nothing else wants the register. It is also OS-9's U
register, in the register RISC-V set aside for the purpose.

Only asking for the *size* costs a system call, which `MYRTOS_MODULE` does once
at entry to check the class fits.

`modules/pimpl/` is exactly this, built from two files.

### What C++ still gets wrong

Measured the same way as the table above:

| Construct | Absolute relocations | vtable |
|---|---|---|
| base class, instance on the stack | 0 | no |
| placement new into the data area | 0 | no |
| virtual call, type visible | 0 | no -- devirtualised |
| virtual call, vtable forced | 3 | yes |
| CRTP | 0 | no |
| global instance with a constructor | 1 | `.init_array`, `.sbss` |

**No virtual functions.** A vtable is a table of function pointers, so it holds
absolute addresses. The two zero rows above are misleading: in one the compiler
proved the dynamic type and devirtualised, in the other the vtable was emitted
in a different object file -- which is still part of the module. Use CRTP where
you want an interface: the base learns the derived type through the template
parameter, the call binds at compile time, and no vtable exists.
`modules/cxxdemo/` shows the pattern.

**No global instances.** A constructor at file scope leaves a pointer in
`.init_array`, which is the same sort of table, and the object itself in
`.sbss`. The class must also not need a constructor to have run: the block
arrives zeroed, and nothing calls one.

**A `static` data member is the easy one to miss.** It looks like part of the
class, so like per-instance state, but it is shared storage -- for every
instance, and therefore for every process. `static __thread` fixes it as it
fixes any other static.

Nesting itself costs nothing. A nested class is a matter of naming, with no
runtime representation of its own, so the same two rules apply to it and nothing
more. Measured: nested as a member, nested with its own instance, a local class
inside a function, and three levels of nesting with CRTP are all clean. Only the
nested class with a virtual function and the one with a plain `static` member
fail, and they fail for the reasons above rather than for being nested.

## Real-time modules

A module's process memory -- its data, stack and thread-local block -- comes
from PSRAM by default, because SRAM is the scarce one and PSRAM is eight
megabytes. Mark a module `RT` in `myrtos_add_module` and it gets SRAM instead:

```cmake
myrtos_add_module(sh modules/sh/sh.c RT)
```

It sets a bit in the attributes byte of the header, which already existed and
held only one. The kernel honours it when allocating the process, and when
copying a module from the card.

Measured before assuming, with the same source built both ways, four runs each:

    stack in SRAM    3157114  3157098  3157146  3157140
    stack in PSRAM   3157108  3157141  3157132  3157086

No difference at all. The XIP cache absorbs it, because a stack is a small
working set touched over and over -- the trap frame is 144 bytes at the same
addresses every system call.

That is not a general claim about PSRAM. A framebuffer streamed by DMA is the
opposite case: large, and every byte touched once, which is what a cache cannot
help with. Mark things RT when their timing matters, and measure the ones that
stream.

## Names are eight characters

The module directory holds names in the 8.3 form a FAT card gives them, so a
module name longer than eight characters cannot work. It used to be cut short
silently: the module built, loaded and registered, and then could not be run,
because no name anyone could type would ever match it. The build refuses it
now, and says what the name would have become.

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

### File-scope variables

`static` or not makes no difference. It changes linkage, not storage, and what
the module format cares about is storage. Measured, both compilers agreeing:

| At file scope | Verdict | Why |
|---|---|---|
| `int g;` | refused | `.bss`, writable and shared |
| `int g = 7;` | refused | `.data`, the same |
| `const int t[] = {...}` | fine | `.rodata`, shared deliberately |
| `const char *n[] = {...}` | refused | a table of addresses |
| `const char n[2][4] = {...}` | fine | characters, no addresses |
| `__thread int g;` | fine | one per process, through `tp` |

The refusals are for two different reasons, and they are different *properties*:

**Position independent** means the module holds no absolute addresses, so it
runs wherever it is loaded. That is about relocations, and it is what
`-mcmodel=medany` buys -- PC-relative addressing with no table to fix up. Note
that the build passes `-fno-pic`: real PIC would reach globals through a GOT,
which is a table of addresses filled in at load time, and that is precisely what
is not wanted here.

**Shareable** means the module holds no writable data, so one copy can serve
every process running it. That is about sections, and no compiler flag has
anything to do with it.

Sharing is the reason it is *wanted*, but not the reason it *breaks*, and the
difference is worth measuring once. A module with `static int counter;` links
like this:

    LOAD  FileSiz 0x12 = 18 bytes      what the image contains
          MemSiz  0x18 = 24 bytes      what the module needs in memory

`.bss` is NOBITS: no file content, just an address and a size, placed six bytes
past the end of what `objcopy -O binary` extracts. Nothing reserves those six
bytes. `module_size` in the header is the file's length, and
`myrtos_moddir_add_copy` allocates exactly that and copies exactly that.

So the variable does not land in shared memory. It lands **outside the module's
memory altogether** -- for a card module, in whatever the heap put after the
allocation; for a resident one, in the next module's header in the concatenated
flash image, where the write is simply lost because flash is not writable.

The format has no concept of `.bss`. `mem_size` is the *process's* block, which
is a different thing entirely.

A module with `static int counter;` is perfectly position independent and simply
cannot be shared. `check_module.py` says so in those words, because calling that
"not position independent" sends the reader looking for the wrong thing.

### Statics under clang

The same, which is the answer worth having. Measured on a `static const` array,
a `static const int` table and a `static __thread int`:

| | gcc | clang |
|---|---|---|
| addressing a `static const` | `R_RISCV_PCREL_HI20`/`LO12` | the same |
| a `__thread` variable | `R_RISCV_TPREL_*` | the same |
| absolute addresses | none | none |

So `-fno-pic -mcmodel=medany` means the same thing to both: statics are reached
PC-relatively, and per-process variables go through `tp` exactly as this document
describes. Nothing about the module format changes.

What must be refused still is, under both:

    static int counter;                  .bss -- writable, and two processes
                                         sharing the code would share it
    static const char *const names[]     R_RISCV_32 in .rela.rodata

A writable static is not a relocation problem and no compiler flag fixes it. It
is refused because the module is *shared*, and that is a property of how myrtos
loads it rather than of how the code was built.

### Clang, and the relative vtables that would fix this

Tried, and worth writing down. Homebrew's LLVM 23 has a riscv32 backend, and
**every C module here compiles with it and passes `check_module.py`** given two
flags beyond the ones GCC gets: `-std=gnu23`, because clang defaults to an older
C where `bool` is not a keyword, and `-fno-jump-tables`.

That second one is the interesting failure. `sh` came out with 97 `R_RISCV_32`
in `.rela.rodata`, all pointing at one label: clang had turned a `switch` into a
table of absolute addresses. It is the same shape as a hand-written table of
function pointers and refused for the same reason. Worth noting that **GCC is
not given `-fno-jump-tables` either** -- it has simply not emitted one yet, which
is luck rather than design. The checker would catch it, as a puzzling build
failure rather than a bug.

On vtables, clang has `-fexperimental-relative-c++-abi-vtables`, which is real
and does exactly what it says. Measured on a virtual call the compiler cannot
devirtualise:

| | vtable entries |
|---|---|
| gcc | `R_RISCV_32` x3 -- absolute |
| clang | `R_RISCV_32` x3 -- absolute |
| clang, relative vtables | `R_RISCV_PLT32` x3, addends 0, 4, 8 |

`R_RISCV_PLT32` is PC-relative: what is stored is the distance from the vtable
slot to the function, so the table needs no fixing up wherever it lands. It is
`MYRTOS_RELTAB_*` in the ABI header, done by the compiler instead of by hand.

**GNU ld cannot link it** -- "internal error: unsupported relocation error".
`ld.lld` links it without complaint, and the result is what it claims to be: no
relocations left in the module at all, and the vtable holding 0x7a, 0x92, 0x82
where absolute addresses would have been around 0x100xx. Distances, not
addresses.

So it works, and the price is a second toolchain for modules: clang to compile,
lld to link. Not a flag; a decision.

**It also found a hole in `check_module.py`, which is the better prize.** The
first run passed. It should not have -- and it did not pass because the
relocations were acceptable, but because the checker looked for `R_RISCV_\w+`
in each line, and this readelf prints an unknown type as `unrecognized: 3b`.
Nothing matched, so the lines were skipped in silence. A tool written to refuse
absolute addresses was quietly ignoring every kind it could not name. It now
reads the type as the third field whatever it says, and refuses anything not on
the list, which is how it should always have worked.

(The flag is Clang's alone. GCC has never had it, in any version -- a confident
web answer says otherwise and is wrong about which compiler.)
