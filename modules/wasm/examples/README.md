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

The host implements twelve WASI calls: `fd_write`, `fd_read`, `fd_close`,
`fd_seek`, `fd_fdstat_get`, `fd_filestat_get`, `path_open`, `fd_prestat_get`,
`fd_prestat_dir_name`, `environ_get`, `environ_sizes_get` and `proc_exit`.
That is enough for stdio and for files, and files are more than they sound:
there is one preopen, `/`, so `/sd/data.txt` and `/dev/acm` arrive the same way.
**A dongle is a file.** hibou.c opens the BleuIO with `open("/dev/acm", O_RDWR)`
and talks to it with `read` and `write`, and an FTDI cable would work the same
day a driver registers one.

Not implemented, and each is small when something needs it:

  - `clock_time_get` -- any notion of the time of day, and `time()`
  - `poll_oneoff`    -- `sleep()`, and waiting on more than one thing at once
  - `args_get`, `args_sizes_get` -- so a program gets no argv. Hardcode paths.
  - `random_get`     -- Rust's standard library wants this for its hash seeds

A read of a device blocks until there is something, which is why hibou.c has no
timer and no polling: the loop simply reads. On the module side that same wait
is `myrtos_arm` and a pulse; here it is one blocking call, and shorter for it.
