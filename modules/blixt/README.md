# Blixt

A Thumb-2 assembler in a Borland-style editor: a menu bar on top, the source
in a blue window, the keys on the bottom line, and F9 to assemble and run what
is on the screen.

    blixt [FILE]

| Key | |
|---|---|
| F1 | help (also the instruction and system-call pages under Help) |
| F2 / F3 | save / open |
| F4 | the listing beside the source, address and code per line, kept current as you type |
| F9 | assemble, write the program to `/tmp` as a module, and run it |
| Alt-F9 | assemble only, and say how it went |
| F10, Esc, Alt-letter | the menus |
| Alt-X | leave |
| Ctrl-B, Ctrl-S, Ctrl-O, Ctrl-Q | F9, F2, F3 and Alt-X for a terminal without them |

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
