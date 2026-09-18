# Applications as .wasm files

A program here is an ordinary program. It is not a UbiqOS module, and none of
the rules in [writing-modules.md](../../../docs/writing-modules.md) apply to it:
no `__thread`, no ban on writable statics, no position independence, no module
header. A wasm program has its own linear memory and its own globals, so what
this machine cannot provide with hardware the interpreter provides with a
sandbox.

The cost is one shared 190 kB interpreter, and nothing per program.

## What the host provides

Twenty WASI calls, implemented in [../wasi.c](../wasi.c):

    fd_write   fd_read    fd_close    fd_seek      fd_fdstat_get
    fd_filestat_get       path_open   fd_prestat_get
    fd_prestat_dir_name   environ_get environ_sizes_get
    proc_exit  clock_time_get         poll_oneoff
    args_get   args_sizes_get         random_get
    path_filestat_get     path_unlink_file       fd_fdstat_set_flags

That is enough for stdio, for files, and for waiting. Files are more than they
sound: there is **one preopen, `/`**, so `/sd/data.txt` and `/dev/acm` arrive
through the same call. **A dongle is a file.** `hibou.c` opens the BleuIO with
`open("/dev/acm", O_RDWR)` and talks to it with `read` and `write`, and an FTDI
cable would work the same day a driver registers one.

`sleep()` and `nanosleep()` work, through `poll_oneoff`. `time()` works but
counts from boot: there is no calendar on this machine and nothing sets a date.

A read of a device blocks until there is something. That is why none of these
examples poll: the loop simply reads. On the module side the same wait is
`ubiqos_arm` and a pulse; here it is one blocking call, and shorter for it.

A program gets its arguments: `wasm /sd/argrand.wasm one two three` arrives as
four, with the path as `argv[0]`. And `getentropy()` works -- the kernel reads
the ring oscillator's random bit and mixes in the microsecond timer, which is
entropy enough to seed a hash or pick an identifier and is not a key. Rust needs
both of these before `main` runs, whether or not the program mentions them.

Not implemented, and small when something needs it: polling several descriptors
at once. A `poll_oneoff` on a descriptor is answered as ready without looking,
which is right while every read blocks.

## Building

The clang route needs the WASI sysroot and the compiler-rt builtins:

    brew install wasi-libc wasi-runtimes

`zig cc` needs neither -- it carries both -- and is below if you would rather
install one thing than three.

`wasi-runtimes` is the easily missed half. Without it the link fails looking for
`libclang_rt.builtins.a`, and its version has to match the LLVM in use.
Homebrew's LLVM already targets wasm32, so there is no other compiler to fetch.

    SYSROOT=$(brew --prefix wasi-libc)/share/wasi-sysroot
    CLANG=$(brew --prefix llvm)/bin/clang

### C, against POSIX -- hibou.c

The ordinary way: `open`, `read`, `write`, `printf`.

    $CLANG --target=wasm32-wasip1 --sysroot=$SYSROOT -Os hibou.c -o hibou.wasm

### C, with no libc at all -- tiny.c

The WASI calls imported by hand. Fourteen times smaller, and no longer portable
C: it will not build for the host and cannot use a library that expects a libc.

    $CLANG --target=wasm32 -Oz -fno-builtin -nostdlib \
           -Wl,--no-entry -Wl,--export=_start -Wl,--strip-all \
           tiny.c -o tiny.wasm

`-fno-builtin` is not optional: without it clang recognises the hand-written
string-length loop and replaces it with a call to `strlen`, which is not linked.

### C, with zig instead -- and nothing to install

`zig cc` is a complete C compiler with wasi-libc and the compiler-rt builtins
already inside it. No sysroot, no `brew install`, no version to keep in step
with an LLVM: one binary builds all three C examples here.

    zig cc --target=wasm32-wasi -Os -Wl,-z,stack-size=65536 \
           hibou.c -o hibou.wasm

**That stack-size is not optional.** Zig asks for a sixteen megabyte wasm stack
by default, which lands in the file as 257 pages of initial linear memory -- and
the host answers `load failed ... memory allocation failed`, because the heap it
has to give is four megabytes and the board has eight in total. Sixty-four
kilobytes brings the file back to two pages and it runs. It is the only
difference between the two toolchains that matters, and it costs a puzzled
minute to find because the error names memory rather than the stack.

The libc-free build works the same way, with `--target=wasm32-freestanding`:

    zig cc --target=wasm32-freestanding -Oz -fno-builtin -nostdlib \
           -Wl,--no-entry -Wl,--export=_start -Wl,--strip-all \
           tiny.c -o tiny.wasm

Zig's wasi-libc is the smaller of the two, and by a margin worth knowing:

    hibou.c     13813 with clang and Homebrew's wasi-libc
                 8096 with zig cc
    argrand.c   23823 clang        24291 zig
    tiny.c        925 clang          932 zig

So the difference is all in the libc, and it shows up only where the libc is
actually used. Both were run on the board, against the dongle.

### Nim -- hibou.nim

Nim reaches wasm the way it reaches everything else, by writing C and driving a
C compiler, so it goes all the way in one command.

    nim c --cpu:wasm32 --os:any --mm:arc -d:useMalloc -d:release --opt:size \
          --noMain --cc:clang \
          --clang.exe:$CLANG --clang.linkerexe:$CLANG \
          --passC:"--target=wasm32-wasip1 --sysroot=$SYSROOT -fno-builtin \
                   -D_WASI_EMULATED_SIGNAL -ffunction-sections -fdata-sections" \
          --passL:"--target=wasm32-wasip1 --sysroot=$SYSROOT \
                   -lwasi-emulated-signal -Wl,--gc-sections -Wl,--strip-all" \
          --out:hibou.wasm hibou.nim

Four of those are load-bearing and each cost an attempt. `-d:useMalloc`, because
`--os:any` has no memory manager and Nim stops with "Port memory manager to your
platform". `_WASI_EMULATED_SIGNAL`, because Nim's `system` module reaches for
`SIGINT` and `SIGSEGV` and wasm has no signals. `--noMain` with the entry
exported as `main` rather than `_start`, because wasi-libc's crt1 owns `_start`
and exporting it here is a duplicate symbol. And `--mm:arc`, which is what keeps
the runtime small enough to be worth measuring.

### Rust

`hello.wasm` in the parent directory was built this way. The target is already
installed; `rustup target add wasm32-wasip1` if it is not.

    rustc --target wasm32-wasip1 -O prog.rs -o prog.wasm

Ordinary `std` works: `std::fs::read_to_string` and `File::open` both do, and so
does anything that seeds a hash map -- `random_get` is there now.

### Not emscripten

`emcc -sSTANDALONE_WASM -sPURE_WASI=1` builds cleanly and imports only four
calls, all of which are implemented here -- but **its `open()` never reaches
WASI**. There is no `path_open` in the import list at all, and a program answers
as though the file were missing. Measured on 3 Sep 2026 against a real file
under `wasm3 --dir /`: it said "no dongle". Since the point of these programs is
that a device is a file, emscripten is the wrong tool for them.

### Arguments and entropy -- argrand.c

    $CLANG --target=wasm32-wasip1 --sysroot=$SYSROOT -Os argrand.c -o argrand.wasm

Prints its argv and sixteen random bytes. Seven imports, and the smallest useful
check that the host gives a program what a language runtime expects.

### TinyGo

`tinygo build -target=wasi -o prog.wasm prog.go` should work on the same
fourteen calls. Not tried here -- the four measured below were, on the board.

## An editor: Atto, and a curses small enough to carry

[Atto](https://github.com/hughbarney/atto) is an emacs in about two thousand
lines of C. It builds and runs here, edits a file on the card and saves it back.

    git clone --depth 1 https://github.com/hughbarney/atto.git
    zig cc --target=wasm32-wasi -Os -funsigned-char -Wl,-z,stack-size=131072 \
           -I curses atto/*.c curses/curses.c curses/compat.c -o atto.wasm

`-funsigned-char` is not optional. Atto guards its insert with `*input > 31`,
and on a signed char an a-ring is negative -- every accented character came back
"Not bound".

61 kB. The only thing in Atto that needs an operating system is curses, and WASI
has no ncurses -- terminfo is a database describing terminals none of which are
here. But a program like this does not want a terminal database: it wants to
move the cursor, write text, clear to the end of a line and read a key, and each
of those is an escape sequence the console already understands.

**The machine speaks UTF-8**, so a program that calls `setlocale` and counts
bytes needs nothing translated on its way in or out. That was not true until
this editor asked for it: the keyboard sent Latin-1 and the console's font was
indexed by the same byte, and Atto read an a-ring as the start of a three-byte
sequence. The seam that briefly sat in `curses.c` is gone; the console decodes
UTF-8 and the keyboard sends it.

Two things a curses shim gets wrong until someone uses it. **Carriage return
has to become newline** -- that is what curses does on input unless a program
calls `nonl()`, and Atto inserts a line break on 10 while answering "Not bound"
to 13, which is what a keyboard sends. And **the screen is not a size you can
assume**: a guest has no ioctl and no terminal to ask, so the host publishes the
console's grid as `LINES` and `COLUMNS` in the environment and `initscr` reads
them there. Thirty by eighty on a screen of forty by a hundred and six is a
third of the screen left dark.

So `curses/` is not a port. It is the twenty-one calls Atto uses, written
directly over ANSI, in about 140 lines. Any other curses program that stays
inside them runs for the same reason.

Two things made it easy, and they are worth knowing before choosing a program to
bring across. Atto reads **raw bytes** and matches escape sequences in its own
key table, so there is no `KEY_UP` to synthesise and no terminfo to consult. And
output is **buffered until refresh** -- that is not an optimisation but the
difference between a usable editor and one that visibly crawls, since a redraw
is a few thousand characters and one write syscall each would be a few thousand
traps.

Filename completion on TAB works, and getting there took three separate
things. Atto shells out to `echo prefix* >tmpfile` and reads the names back, so
`curses/compat.c` supplies both halves a wasm guest has no way to do: `mkstemp`
really makes a file in /tmp -- a failed one calls Atto's `fatal()` and takes the
editor down -- and `system` recognises that one command shape, expands the
pattern itself with `opendir`/`readdir`, and writes the names where the shell
would have put them. Atto never learns the difference.

Under it, the host needed `fd_readdir`, and two things about it are worth
knowing before writing one. A dirent header starts with a 64-bit field, and
storing it with a plain 64-bit write traps on Hazard3 with mcause 6 unless the
buffer happens to be aligned -- a guest chooses that address, so it does not.
And `d_ino` must not be left zero: it looks like the honest answer for a
filesystem that has no inodes, but wasi-libc reads zero as "unknown, go and find
out", calls `fstatat` relative to the directory descriptor, and silently drops
every entry whose lookup fails. Twenty entries went into the buffer and none
came out. A hash of the path is stable, distinct, and never zero.

The third was `unlink`. Atto opens its temp file, unlinks it at once so nothing
survives however it exits, and only then reads through the descriptor it kept.
POSIX keeps such a file alive until the last close; a FAT directory entry has
nowhere to record "gone but still open". So the host holds the removal until the
descriptor closes, and sweeps whatever is left when the program exits.

`wls.c` is the small program that made this findable: `ls` and nothing else, so
that when a directory listing comes back empty there is only one place to look.

## Reading a program's imports

Before copying a new binary across, check what it actually asks for. Twenty
lines of Python walking section 2 of the file beats installing a toolchain:

    python3 - prog.wasm <<'PYEOF'
    import sys
    d = open(sys.argv[1], 'rb').read()
    def uleb(b, i):
        r = s = 0
        while True:
            x = b[i]; i += 1; r |= (x & 0x7f) << s; s += 7
            if not x & 0x80: return r, i
    i = 8
    while i < len(d):
        sid = d[i]; i += 1
        size, i = uleb(d, i)
        if sid == 2:
            j = i; n, j = uleb(d, j)
            for _ in range(n):
                l, j = uleb(d, j); mod = d[j:j+l].decode(); j += l
                l, j = uleb(d, j); nm  = d[j:j+l].decode(); j += l
                k = d[j]; j += 1
                if k == 0: _, j = uleb(d, j)
                elif k in (1, 2):
                    if k == 1: j += 1
                    lim = d[j]; j += 1; _, j = uleb(d, j)
                    if lim: _, j = uleb(d, j)
                elif k == 3: j += 2
                print(f"  {mod}.{nm}")
        i += size
    PYEOF

## Getting one onto the board

    usbdisk                 # the card becomes a disk on the host
    # copy the .wasm across, then eject it there
    usbdisk off
    wasm /sd/hibou.wasm

Long names work in both directions: the filesystem reads and writes VFAT long
entries, so `hibou.wasm` is `hibou.wasm` and not `HIBO~1.WAS`, whether it was
copied from the host or created on the board.

`wasm` with no argument runs the program built into the module. An argument
beginning with `/` is a path; anything else is a stage name -- `entry`, `bss`,
`heap`, `env`, `runtime`, `parse`, `load`, `link` -- which stops after that step.
That exists because bisecting a host fault that way found one already.

## What one costs

Measured 3 Sep 2026. All four do the same job against the dongle, and all four
were run on the board -- these are the sizes of programs that work, not of
programs that link:

    tiny.wasm       925   C, raw WASI calls, no libc
    hibou.nim      4258   Nim, raw WASI calls, ARC, one command
    hibou.wasm    13813   C, the same program against POSIX and stdio
    hibouair.mod   3124   the native module -- and it decodes and draws a table

The third number is the surprising one, and it is worth knowing where it goes.
Almost all of it is a single function of 6.9 kB: **dlmalloc**. Opening a file
makes wasi-libc build its preopen table on the heap, so any program that touches
a file pays for the allocator. Dropping stdio for plain `write()` saved two
kilobytes; dropping libc altogether saved twelve.

So a program that talks to a device can be smaller than the module it replaces,
and one that wants printf and the standard library will not be. Nim sitting
between the two is the pleasant surprise: a garbage-collected language with its
runtime, in four kilobytes, because it never touches dlmalloc either.

## Two things that cost a run each

A raw `path_open` takes a path **relative to the preopen, with no leading
slash** -- the host puts the slash back itself. `"/dev/acm"` opens something
that is not the dongle and reads nothing, which looks exactly like a dongle that
is not plugged in. libc does that stripping for you; by hand you must.

A device wants time between commands. The BleuIO echoes the first and ignores
the rest if `ATE0`, `AT+CENTRAL` and `AT+FINDSCANDATA` go out back to back. The
module had `ubiqos_sleep(200)` between them all along, and the first wasm port
had nothing to sleep with. That is what `poll_oneoff` is for.
