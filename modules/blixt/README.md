# Blixt

A Thumb-2 assembler in a Borland-style editor: a menu bar on top, the source
in a blue window, the keys on the bottom line, and F9 to assemble and run what
is on the screen.

    blixt [FILE]

| Key | |
|---|---|
| F1 or Ctrl-H | help (also the instruction and system-call pages under Help) |
| F2 or Ctrl-S / F3 or Ctrl-O | save / open |
| F4 or Ctrl-T | the listing beside the source, address and code per line, kept current as you type |
| F9 or Ctrl-R | assemble, write the program to `/tmp` as a module, and run it |
| Alt-F9 or Ctrl-K | assemble only, and say how it went |
| F10, Esc, Alt-letter | the menus |
| Alt-X or Ctrl-Q | leave |
| Compile > Destination | Memory (run and keep nothing) or Disk (also write NAME.mod beside the source) |

Every function key has a control key, because a Mac keeps its function keys
for itself, over SSH and through Keybridge alike. Ctrl-H is free for help
because the board's keyboard sends DEL (127) for Backspace, as terminals do.

The screen is plain ANSI: cursor positioning, sixteen colours and UTF-8. It is
kept in memory and only the cells that changed are sent, in one write, so it
is as quick over USB or SSH as on the board's own screen. The frames are drawn
with box characters that the console's font has in slots 128 to 159.

## The assembler

`asm.c`, UAL syntax as GNU as reads it, for the Cortex-M33. Where two encodings
fit it takes the one GNU as takes, which is the shorter one unless the
mnemonic says `.w`. A reference forward is first assumed short and grows when
a later pass finds it out of reach; when a pass changes nothing, one more
writes the code and the errors.

One difference is on purpose: a constant used before its `.equ` makes GNU as
take the 32-bit form, because it cannot go back; Blixt goes back and takes the
16-bit one when it fits. The tests define their constants first.

`.include "file.s"` assembles another file in place. It is looked for beside
the file that includes it, then beside the source, then as written; an error
in it is reported as "file.s line N" on the `.include` line.

`test/run.sh` assembles every `test/*.s` with Blixt and with
`arm-none-eabi-as` and compares the bytes line by line:

    modules/blixt/test/run.sh

## A program

It starts at `main:` (or `_start:`, or its first byte) with r0 = argc and
r1 = argv, and ends by returning. A system call is the number in r7, the
arguments in r0 to r2, and `svc 0`. `.word label` and `ldr rX, =label` become
entries in the module's relocation table, so they hold real addresses when the
program runs.

## On a Mac

Everything but running works in a terminal, which is where most of it was
tried. F9 there writes `NAME.mod` next to the source.

    cc -std=gnu11 -O2 -o blixt modules/blixt/{blixt,tui,asm,modwrite,plat_posix}.c

`test/drive.py` runs it in a pseudo-terminal, or on a board's console with
`--serial`, types keys and prints the screen it gets back.
