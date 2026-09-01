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

**It works, and here is the recipe.** `gp`-relative addressing on RISC-V is not
a compiler mode -- the compiler emits PC-relative and the *linker* rewrites it,
when the target is within gp's reach of `__global_pointer$`. So it takes a
linker script rather than a flag:

```ld
. = 0x1000;                 /* out of x0's reach, or the linker uses x0 */
__global_pointer$ = 0x1000;
.sdata : { *(.sdata .sdata.*) }
.sbss  : { *(.sbss .sbss.* .bss .bss.*) }
. = 0x10000;
.text   : { *(.text .text.*) }
.rodata : { *(.rodata .rodata.*) }
```

`static int counter; int bump(void){return ++counter;}` then links to

    lw   a0,0(gp)
    addi a0,a0,1
    sw   a0,0(gp)

with no relocations left at all. Set `gp` per process and the same shared code
reaches different memory. Data at `0x0` does *not* work: the linker relaxes to
`0(zero)` because the address is directly encodable, so it must sit above x0's
2 kB window. And gp reaches +/-2 kB, so a module's whole writable state has to
fit in about four kilobytes -- which is what `mem_size` is anyway.

**But it would be a downgrade, and the reason is the reach.** `gp` puts the
offset in one instruction's twelve bits, so it covers 2 kB either side and no
more. `__thread` has no such limit: TPREL is `HI20`/`LO12`/`ADD`, a full 32-bit
offset, and a sixty-kilobyte thread-local array compiles without complaint.

    gp-relative   one instruction    +/-2 kB      static int x;
    tp-relative   one to three       32 bits      static __thread int x;

So the mechanism already in use is the more capable one. `gp` would buy the
convenience of writing plain `static` and cost the range -- and it would fail
badly rather than gracefully: data past 2 kB simply does not get relaxed, and
the linker leaves a PC-relative reference pointing into the module image. Only
the writable-section check would catch it.

What stands in the way is one thing, and it is written down at `kernel_gp` in
scheduler.c: the trap vector restores the process's `gp` on the way in, and the
kernel's own C code needs its own. The vector would have to swap to `kernel_gp`
on entry, which is a few instructions.

**`lwipd` is proof that the mechanism works and a warning that the last step is
missing.** It is linked with `__global_pointer$` at 0x11793994, its own data in
PSRAM, and has 421 `gp`-relative accesses. The kernel starts every process with
`frame->gp = kernel_gp`, which is 0x2007f633 in SRAM -- so every one of those
accesses would land in the kernel's small-data area. It has never been run
successfully, so nobody found out.

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

### __thread, and where it stops

The way out is `__thread`, and it is not a workaround but the mechanism the
format was built around -- it is OS-9's U register, in the register RISC-V set
aside for it. Three forms were measured, and all three pass as *position
independent and shareable*:

    __thread int counter;        // uninitialised, into .tbss
    __thread int seeded = 5;     // initialised, into .tdata, copied per process
    __thread char buf[256];      // an aggregate is no different

Two forms do not, and both fail at compilation rather than at the checker, which
is the better place:

    __thread int x;
    static int *p = &x;
    // error: initializer element is not constant

A thread-local address is not known until the process exists, so it cannot stand
in a static initialiser. And in C++:

    struct Counter { int n; Counter() : n(7) {} };
    __thread Counter c;
    // error: non-local variable 'c' declared '__thread'
    //        needs dynamic initialization

There is no phase in which a constructor could run -- nothing happens before
`module_main` -- so thread-local objects must be plain data. A POD is fine.

**What it costs.** The thread-local block is carved out of the process's memory
and charged whether the variable is touched or not. `errno` and eight `FILE`
objects come to 228 bytes; `strtok`'s saved pointer takes it to 232. Against the
default four kilobytes that is small, and `MYRTOS_MEM_SIZE` raises the ceiling
when it is not.

**strtok is the one that catches people out.** Written the usual way it keeps a
`static char *` between calls, and the module is refused:

    MODULE IS NOT SHAREABLE:
      d.elf: writable section .sbss is present

A program that brings its own `strtok` is refused for exactly that reason.
`myrtos_string.h` keeps that state in `__thread` instead, and offers `strtok_r`,
which keeps it in the caller's own variable and needs nothing hidden at all.

**The rule of thumb: `__thread` for code you write, `SINGLE` for code you
import.** Putting `__thread` in front of every global in a third-party file is
exactly the editing that porting was meant to avoid, and a `SINGLE` module is
allowed writable data with no change to the source at all -- at the price of one
instance running at a time, which for a utility is no price.

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

### And now it is allowed through

Closing that hole closed the door on the thing that prompted it: the checker
refused what it could not name, and what it could not name was `R_RISCV_PLT32`.
Both halves were right and the result was wrong.

So `R_RISCV_PLT32` is on the position-independent list, and the checker maps
type `0x3b` to that name itself rather than relying on the reader. `llvm-readelf`
names it; binutils `readelf` does not; which one happens to be installed is no
basis for deciding whether a module may be loaded.

A class with three virtual methods and no static instance now gets:

    clang++ --target=riscv32-unknown-elf \
        -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -mabi=ilp32 \
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2 \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        -fexperimental-relative-c++-abi-vtables -c vt.cpp -o vt.o
    ld.lld -m elf32lriscv -N -e 0 -o vt.elf vt.o

    vt.elf: position independent and shareable

The same source through gcc is refused on `.rela.rodata._ZTV4Base`. **That is
the first C++ with virtual functions this module format can accept at all.**
Every existing module still passes unchanged.

One caution that has nothing to do with vtables: a **static instance** of such a
class puts an absolute vtable pointer in its own `.data`, and is writable data
besides. Relative vtables fix the table, not the object.

The C flags are unchanged and still both needed. Re-measured on `sh`: 97
`R_RISCV_32` without `-fno-jump-tables`, none with it.

## Choosing the compiler, per module

Both toolchains are wired in. `myrtos_add_module` is GCC and is the default;
`myrtos_add_clang_module` takes the same arguments, including `RT` and `SINGLE`,
and builds with clang and `ld.lld` instead. The two produce the same `${name}.mod`
through the same `check_module.py`, `objcopy` and `make_module.py` -- nothing
downstream can tell which compiler made one.

    myrtos_add_module(echo modules/echo/echo.c)              # gcc
    myrtos_add_clang_module(cxxdemo modules/cxxdemo/cxxdemo.cpp)

**There is one reason to choose clang and it is virtual functions.** For plain C
there is nothing to gain, and GCC is the road everything else travels.

CMake has one compiler per project, so the clang modules are built by custom
commands rather than `add_executable`. The architecture flags are taken from
`CMAKE_C_FLAGS`, so the two compilers cannot drift apart, and clang is *probed*
at configure time with those flags: a machine without clang and lld, or with a
clang that cannot build for this target, silently gets the GCC path instead of a
broken build. Configure says which:

    -- Clang module toolchain: /opt/homebrew/opt/llvm/bin/clang + /opt/homebrew/bin/ld.lld

`ld.lld` is a separate Homebrew formula from `llvm` and is **not** in the llvm
formula's own `bin`. Note also that lld rejects `--no-warn-rwx-segments`, which
GNU ld needs: it does not warn about them at all.

Verified on the board rather than at the build: a clang-built module was made
resident, flashed and run, and printed what it should.

## Ordinary C, through myrtos_posix.h

`common/myrtos_posix.h` gives `open`, `read`, `write`, `close` and `lseek` their
POSIX names and shapes, so a file-handling loop can be built here unchanged.
`modules/cat/cat.c` is written against it and has nothing myrtos-shaped in its
loop at all.

**Functions, not macros, and the difference is not taste.** A macro rewrites
every occurrence of the name, and this repository has a driver struct whose
members are `open`, `read`, `write` and `close`, and a C++ class with a `write`
method. `#define write(...)` breaks both. Functions cannot, and they may carry
these names safely because modules are built `-nostdlib`: there is no C library
here to collide with.

**It refuses rather than pretends.** `O_APPEND` needs the file's length and
there is no `stat`, so it returns -1 with `ENOSYS` instead of quietly writing at
the start. `SEEK_END` likewise. `O_TRUNC` is honoured by removing the file
first, which is what `write` has always done by hand, and `O_CREAT` is already
implied because the first write brings a file into being.

**errno is thread-local, and must be defined once by the program:**

    __thread int errno;

exactly as a C library would define it for you. It cannot be a plain static: a
shareable module may not have writable data. In `cat` it costs four bytes of
`.tbss`, and each process running the module gets its own -- which is more
nearly right than a global errno would be in a single address space.

Still missing before a real port: `FILE` and stdio, `malloc`, `string.h`,
`ctype.h`. And the larger obstacle is not the API but the module format -- see
"The other rule" above. Code with file-scope variables comes in as a `SINGLE`
module, which is allowed writable data at the price of one instance at a time.

## stdio, and where a FILE lives

`common/myrtos_stdio.h` gives `fopen`, `fclose`, `fread`, `fwrite`, `fgetc`,
`fputc`, `fgets`, `fputs`, `puts`, `fflush`, `fseek`, `ftell`, `setvbuf`,
`feof` and `ferror`. `modules/head/head.c` is written against it and has nothing
myrtos-shaped in it but the include and the entry point's name.

**The FILE objects are thread-local and the buffers are not**, and that split is
the whole design. A FILE is twenty-eight bytes; eight of them plus errno cost
`head` 228 bytes of `.tbss`. Cheap -- and, more to the point, it makes `stdin`,
`stdout` and `stderr` addressable with no initialisation, which matters because
nothing runs before `module_main`. Objects that had to be constructed first
could not be printed to.

A buffer is another matter: it is charged to the process whether it opens a file
or not, and 512 bytes is an eighth of the default four kilobytes. So buffers
come from PSRAM through `myrtos_alloc_bulk`, which is the bulk data that pool
exists for, and a stream whose buffer cannot be had still works a byte at a
time. `setvbuf` takes the program's own array instead.

The program owes one line, as it owes a C library one:

    MYRTOS_LIBC_DEFINE

which defines errno, the stream table, and what strtok remembers between calls.

**Modes are "r", "w" and "a".** Appending works because `stat` arrived and
`open` can ask how long the file already is; before that it was refused, since a
program that asks to append and is given the start of the file destroys it.

`"+"` is still refused with `ENOSYS`. It needs a stream that can turn round
mid-way, and this one holds a single direction at a time -- which is what keeps
the buffer arithmetic simple enough to be right.

**The flags are the kernel's, not the shim's.** `myrtos_open_flags(path, flags)`
takes POSIX's own numbers, and the filesystem server acts on them: it refuses a
file that is not there unless one of the creating flags is given, empties one
for `O_TRUNC`, and puts the descriptor at the end for `O_APPEND` -- all before
the caller ever holds it. `myrtos_posix.h` aliases the constants rather than
translating them, and its `open` is a pass-through.

That matters beyond tidiness. A caller that arranges truncation and appending
for itself holds, for a moment, a descriptor pointing at the wrong place; and
`myrtos_open` on its own could not refuse a missing file, so `cat` had to tell
one from an empty file by the sign of a return value. Both are gone.

`stat(path, &st)` gives `st_size` and `st_mode`, with `S_ISDIR`. There are no
owners, times or permissions here to report, so those fields do not exist rather
than lying. `lseek(fd, n, SEEK_END)` is still refused, and the reason is no
longer the missing stat: a raw descriptor does not carry its path, so there is
nothing to ask about. `fopen` knows its path, which is why `"a"` works.

## The rest of the C library

`myrtos_string.h`, `myrtos_ctype.h` and `myrtos_stdlib.h` alongside the stdio
header, all inline and none of them holding state -- so a module pays only for
what it calls and there is nothing for the position-independence check to
object to.

`printf`, `fprintf`, `snprintf`, `sprintf` and their `v` forms share one
formatter: a stream and a fixed buffer differ only in where a character goes, so
the sink is the difference and nothing is written twice. The count is kept
whether or not the buffer can take it, which is how `snprintf` answers the
standard's question -- how long would it have been. `%d %i %u %x %X %o %c %s %p`
with `-`, `0`, `+`, space, width, precision and `*`. `l`, `h` and `z` are read
and ignored, because long is int here and ported code is full of them.

`sscanf`, `fscanf` and `scanf` mirror it with a source instead of a sink, and
`ungetc` is what makes them possible: reading a number means reading one
character too many.

`malloc` asks PSRAM first. A process's own pool is four kilobytes -- a stack and
a few locals, not what ported code expects of malloc -- while the bulk pool has
megabytes and is what it is for. Eight bytes in front of every block remember
its size, because the kernel knows it but through no call a module can make, and
`realloc` cannot work without it.

`modules/libctest/libctest.c` checks all of it against what the standard says
and prints ok or FAIL per line, because eyeballing printf output is how a wrong
width goes unnoticed for a year. It is also a worked example of the rule above:
its failure counter had to become `static __thread`, since `check_module.py`
refused the file while it was a plain static.

Still absent: floating point in printf and scanf, `qsort`, `time`, and `"+"`
stream modes.

## Global constructors and destructors: most of the way

Measured, and the mechanism is there:

* The compiler emits `.init_array` already. With `-fno-use-cxa-atexit`, which
  the build now passes, the destructors of globals go to `.fini_array` instead
  of being registered through `__cxa_atexit` -- which would want that function
  and `__dso_handle` from a C library that does not exist here. With the flag
  the object file has **no undefined symbols at all**; without it, two.

* The linker's own script already contains
  `PROVIDE_HIDDEN (__init_array_start = .)` and the other three. They never
  appeared because **PROVIDE only defines a symbol something asks for**. Merely
  referring to them is enough. There is no linker script in this repository and
  none was needed.

  A copy of that script is at `docs/reference/ld-builtin-elf32lriscv.ld`, for
  reading only -- nothing links against it. Modules are linked with no `-T` at
  all, and it is not a file in the Pico SDK either: it is compiled into `ld`.
  `riscv32-unknown-elf-ld -m elf32lriscv --verbose` prints the real one, which
  is what to trust if the copy has aged.

`myrtos_module.h` has `myrtos_run_constructors`, `myrtos_run_destructors` and
`MYRTOS_CXX_MAIN(fn)`, which writes a `module_main` that runs the first, calls
your function, and runs the second -- backwards, as the standard requires. A
ported program needs a shim like that anyway, since the entry point is not
called `main` here.

**A module with global objects must be SINGLE**: the object is writable data and
`.init_array` is a table of absolute function pointers, and a shareable module
may have neither.

**It is not finished.** A demonstration module built this way hung the machine
when run, and the cause is not yet known -- the first guess, that the objects
land in `.bss` outside the image, does not survive reading the loader: a SINGLE
module is copied into a reserved region that `.bss` sits inside, uninitialised
but present. The support above is committed because every part of it was
measured; the demonstration is not, because it does not work. Do not put a
module with global constructors on a card until this is understood.

## Redirection

The shell understands `>`, `>>`, `<` and `2>`, and does what every shell has
done since the seventh edition: it puts the file on the descriptor, starts the
child, and puts its own descriptor back. The child is told nothing and needs to
know nothing -- it writes to 1 as it always did.

That works because a child inherits its parent's numbered paths, and because
files got descriptors. `myrtos_dup(fd, new)` is `dup` and `dup2` in one: -1 for
the lowest free number, or a specific one, closing whatever was there. `dup` and
`dup2` themselves are in `myrtos_posix.h`.

Two descriptors can now name one device, so closing one no longer closes the
driver if another still refers to it. Two naming one open file share its
position, which is what the reference count in the open file table was for.

The filename is copied out of the command line rather than terminated in place:
a NUL in the middle of the line would cut off every argument after it, which is
how `cmd > file arg` would quietly lose `arg`.

**Pipelines go through /tmp**, not through the kernel's pipe. `a | b` writes the
left side into a file in PSRAM and gives it to the right side, which is the
redirection that already works rather than a second arrangement of descriptors.
They run one after the other, so nothing is inherited that should not be, and
the whole difficulty below disappears.

The price is that it does not stream: all of the left side exists before the
right side starts. With eight megabytes of PSRAM and no `yes` to run for ever
that is a fair trade, and the streaming version is an upgrade of this same shape.

**The kernel's real pipe is still there and still unproven.** The kernel has them: `myrtos_pipe(fds)` gives two
descriptors onto a 128-byte ring, a descriptor can name a pipe beside a device
and a file, the reader blocks while it is empty, and an empty pipe whose writers
have all gone reads as end of file rather than blocking for ever -- which the
read system call asks about with `myrtos_io_at_eof` before deciding to wait.

**The shell half is not finished**, and `|` is refused rather than accepted.
It hung the shell twice. One real cause was found and fixed: a child inherits
*every* descriptor its parent holds, and there is no `fork` here and so no
moment inside the child to close what it does not need -- so a reader started
while the shell still held the writing end inherited it, and a reader that holds
the writing end waits for itself. Closing each end as soon as the child's
descriptor has it fixes that, and it is in the code. It was not enough; whatever
else is wrong is not yet known.

The parts that redirection needs and pipes share -- `myrtos_dup`, the device use
count, the open file reference count -- are all proven, because redirection
uses them.

## The namespace

A path names its volume first: `/sd/docs/readme.txt`. The root is owned by
nobody, so listing it lists the volumes -- which is OS-9's arrangement, where
`/d0` and `/h0` sat at the top and the I/O manager dispatched on the name in
front. A process starts in `/`, so a bare `ls` after boot shows volumes and not
files.

`/dev` is the device table `io.c` already keeps, shown as a directory. Only
listing is implemented, and that is not a gap: a device is opened by name
through the I/O manager, not read as a file.

**A device is reached at `/dev/name` and nowhere else.** Bare names used to work
too, and it seemed harmless: `open` asked the device table when a name had no
slash. It was not harmless. A name that matched a device could never be a file,
so `echo hej > null` in any directory wrote to the null device and created
nothing, and six names -- null, term, con, usb, kbd, acm -- were unusable. DOS
had exactly this with CON for twenty years.

`/var/dmesg` is everything the kernel has said, as a file. The boot messages go
to the screen and the UART, and a session on the USB console never sees them:
by the time that console exists the kernel has finished talking. The ring is a
static buffer in `main.c` beside `myrtos_print`, because the first line is
written before any pool exists -- and the earliest lines are exactly the ones an
allocation could not have held. It is read by position rather than as a stream,
so two readers do not interfere and `cat` can be run twice.

**`/dev/null`** is there. Everything written to it is taken and forgotten, and
reading it is immediately the end. That second half needed a new question in the
driver interface -- `at_eof` -- because a read of nothing means "not yet"
everywhere else here, and a reader of `/dev/null` would have waited for ever.
It is the one device registered without a descriptor: a descriptor says which
pins, which speed and which driver, and this has no hardware to describe.

## /tmp

A volume whose files live in PSRAM and go when the power does. Eight megabytes
sit behind the second chip select doing very little, and a scratch file is what
bulk memory is for -- the card is slow, wears out, and may not be there at all.

Flat: no directories, eight files at once. A scratch filesystem that needed a
directory tree would be a filesystem, and there is one of those. It has no
`find_nth`, so module scanning passes it over without a special case -- nobody
should be looking for modules in scratch space.

    echo something > /tmp/p
    cat /tmp/p
    ls /tmp

The reason it was built now is pipelines. `a | b` can be `a > /tmp/p` then
`b < /tmp/p`, which uses the redirection that already works rather than a second
arrangement of descriptors that has to be got right again. That is sequential
rather than streaming -- the whole intermediate exists before the right side
starts -- and for this machine, with eight megabytes and no `yes`, that is a
fair trade.

It also fixed something older. `ls` on an empty directory used to say "no such
directory", because an empty one has no first entry to list and neither has one
that is not there. `stat` can tell them apart, so now it does.

