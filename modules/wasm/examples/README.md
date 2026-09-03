# Applications as .wasm files

A program here is an ordinary C file. It is not a myrtos module: none of the
rules in ../../../docs/writing-modules.md apply to it -- no `__thread`, no ban
on writable statics, no position independence -- because a wasm program has its
own linear memory and its own globals. What the machine cannot provide with
hardware, the interpreter provides with a sandbox.

## Building

    clang --target=wasm32-wasip1 \
          --sysroot=$(brew --prefix wasi-libc)/share/wasi-sysroot \
          -O2 ctest.c -o ctest.wasm

`brew install wasi-libc wasi-runtimes` provides the sysroot and the compiler-rt
builtins; the LLVM that Homebrew installs already targets wasm32.

**Not emscripten.** `emcc -sSTANDALONE_WASM -sPURE_WASI=1` builds and the result
imports only four calls we implement -- but its `open()` never reaches WASI, so
there is no `path_open` in the import list and every file access fails. Measured
on 3 Sep 2026 against a real file with `wasm3 --dir /`: it answered "no dongle".
wasi-libc's `open()` goes straight to `path_open`, which is the whole point.

## Getting one onto the board

    usbdisk              # the card becomes a disk on the host
    # copy the .wasm across, eject it there
    usbdisk off
    wasm /sd/ctest.wasm

Long names work: the filesystem reads VFAT long entries, so `ctest.wasm` is
`ctest.wasm` and not `CTES~1.WAS`. Writing them does not yet, so the copy has to
come from the host rather than from the board.

## What a program may use

The host implements fourteen WASI calls: `fd_write`, `fd_read`, `fd_close`,
`fd_seek`, `fd_fdstat_get`, `fd_filestat_get`, `path_open`, `fd_prestat_get`,
`fd_prestat_dir_name`, `environ_get`, `environ_sizes_get`, `proc_exit`,
`clock_time_get` and `poll_oneoff`.
That is enough for stdio and for files, and files are more than they sound:
there is one preopen, `/`, so `/sd/data.txt` and `/dev/acm` arrive the same way.
**A dongle is a file.** hibou.c opens the BleuIO with `open("/dev/acm", O_RDWR)`
and talks to it with `read` and `write`, and an FTDI cable would work the same
day a driver registers one.

`sleep()` and `nanosleep()` work, through `poll_oneoff`. hibou.c needs them:
the BleuIO wants a pause between AT+CENTRAL and the scan that follows, and
without one the first command is echoed and the rest are ignored. `time()` works
too, but counts from boot -- there is no calendar on this machine and nothing
sets a date.

Not implemented, and each is small when something needs it:

  - `args_get`, `args_sizes_get` -- so a program gets no argv. Hardcode paths.
  - `random_get`     -- Rust's standard library wants this for its hash seeds
  - polling several descriptors at once. A `poll_oneoff` on a descriptor is
    answered as ready without looking, which is right while every read blocks.

A read of a device blocks until there is something, which is why hibou.c has no
timer and no polling: the loop simply reads. On the module side that same wait
is `myrtos_arm` and a pulse; here it is one blocking call, and shorter for it.

## What one costs

Measured on 3 Sep 2026, all three doing the same job against the dongle:

    tiny.wasm       925   raw WASI calls, no libc
    hibou.wasm    13813   the same program written against POSIX
    hibouair.mod   3124   the native module -- and it decodes and draws a table

The middle number is the surprising one, and it is worth knowing where it goes.
Almost all of it is a single function of 6.9 kB: dlmalloc. Opening a file makes
wasi-libc build its preopen table on the heap, so any program that touches a
file pays for the allocator. Dropping stdio for plain `write()` saved two
kilobytes; dropping libc altogether saved twelve.

So a program that talks to a device can be smaller than the module it replaces,
and one that wants printf and the standard library will not be. Neither number
is the interpreter, which is 190 kB and shared by every program that runs.
