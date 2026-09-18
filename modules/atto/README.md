# atto -- an emacs, built natively

Hugh Barney's Atto, about two thousand lines of public-domain C, running as an
ordinary UbiqOS module. `upstream/` holds the editor unmodified and says where
it came from; everything UbiqOS adds is here.

It ran as `atto.wasm` first, and still can -- see
`modules/wasm/examples/README.md`. Native is 3.5 times smaller on the card and
needs no interpreter under it.

## What it takes to build a C program for UbiqOS

Three things, and the size of each is the point:

**The C library.** `NEWLIB` on the module line. Atto was written against a
hosted libc and rewriting it is not the job, so it gets one:
`common/ubiqos_syscalls.c` is the bottom end and the module is copied per
process, which is what makes newlib's `_impure_ptr` correct without anyone
arranging it.

**Curses.** Not ncurses: `modules/wasm/examples/curses`, twenty-one calls over
ANSI escape sequences, written for the wasm build and reused here without a
change. It was already over POSIX rather than over WASI, which is why.

**`compat.c`**, beside this file. `mkstemp`, and the filename completion Atto
asks for by shelling out.

## The one flag that is not optional

`-std=gnu17`. Atto declares `free_other_windows()` with an empty parameter list
and defines it with an argument -- legal K&R, and in C23, which gcc 15 compiles
by default, `()` means `(void)` and it is an error. `-funsigned-char` matters
for a different reason, written up in the wasm README: Atto guards its insert
with `*input > 31`, and a signed char makes every character above 127 negative.
