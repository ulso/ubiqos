# myrtos

A small real-time operating system for RISC-V, in the spirit of OS-9:
**position-independent code** and a **module system** where code is shared
between processes rather than loaded once per process.

The target is the Hazard3 core in the RP2350 on an **Adafruit Fruit Jam**. The
RP2350 has two ARM Cortex-M33 cores *and* two RISC-V cores; the boot ROM reads
`PICOBIN_IMAGE_TYPE_EXE_CPU` from the flash image and switches to RISC-V only if
the image says so. That is why everything is built with
`PICO_PLATFORM=rp2350-riscv`, and why the system has to live in flash — a
RAM-only image gives the boot ROM nothing to switch on.

## What exists

- Pre-emptive scheduling on the machine timer, 1 ms quantum, up to 8 processes
- A 320 kB TLSF heap that splits and coalesces blocks
- Modules loaded from FAT32 on the SD card, or found resident in flash
- A module directory with link counts — a module already in memory is shared
- An I/O manager with device descriptors; console on both UART and USB CDC
- Per-process path numbers inherited across `exec`: 0 stdin, 1 stdout, 2 stderr
- A shell, `sh`, that runs modules with arguments and `argc`/`argv`
- Blocking reads and `wait`, so waiting costs nothing

## Building

Requires the Pico SDK 2.3.0 and a RISC-V toolchain.

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0
export PATH=$HOME/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_2_0_3/bin:$PATH
cmake -S . -B build -G Ninja && ninja -C build
```

This produces `build/os_kernel.uf2` and one `.mod` file per module.

The toolchain above is the one the SDK 2.2.0 installer left behind, and it
works. SDK 2.3.0 prefers `gcc-riscv32-pico-elf`, which can target the core the
board actually has with `-mcpu=hazard3-rp2350`; that is not in use yet.

## Flashing

Hold **BOOTSEL**, press **RESET**, then:

```bash
picotool load -x build/os_kernel.uf2
```

**The J-Link cannot get in.** A Segger J-Link Ultra+ can halt the core and read
and write RAM, but refuses to program flash (`Failed to read back RAMCode`). The
cause has not been established. BOOTSEL and `picotool` work, and that is the
route in use.

## The module system

A module is a file with a header — no ELF, and no loader that relocates it. It
runs where it lies: straight out of flash without being copied, or from a copy
on the heap when it came from the card. The header is defined in
[`common/myrtos_abi.h`](common/myrtos_abi.h) and states, among other things, how
much RAM the process needs: the data area grows from the bottom and the stack
from the top of the same block.

Several processes sharing one copy of the code is the whole point, and it rests
on the code containing no absolute addresses.

### Position independence without a GOT

Modules are built with `-fno-pic -mcmodel=medany`, **not** `-fPIC`. This is
counter-intuitive: `-fPIC` creates a GOT, and the GOT entries are filled with
addresses valid where the module was linked. A module loaded anywhere else then
reads the wrong ones. `medany` instead gives `auipc`-based addressing, which is
genuinely PC-relative.

The requirement is checked at build time. [`check_module.py`](check_module.py)
reads the relocations out of the object files with `readelf -W` and rejects the
module if any allocated section contains an absolute reference such as
`R_RISCV_32`. Without that check the fault first appears as a crash after
loading, at an address that tells you nothing.

### Writing a module

Put the source under `modules/` and register it in `CMakeLists.txt`:

```cmake
myrtos_add_module(mine modules/mine/mine.c)
```

A module starts at `module_main`, which receives the command line:

```c
#include "../../common/myrtos_abi.h"

void module_main(int argc, char **argv) {
    myrtos_line_t line;
    myrtos_line_reset(&line);
    for (int i = 1; i < argc; i++) myrtos_line_str(&line, argv[i]);
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
```

`argv[0]` is the module's own name from its header. The kernel copies the
command line to the bottom of the process's own memory area, splits it there
with quoting handled, and builds the argv vector after the string — no extra
allocation, and nothing to free.

## Getting modules into the system

**From the SD card.** Copy the `.mod` files to the root of the card. The
filesystem support is FAT32 and read-only; the kernel looks for files with the
extension `MOD` and registers them at startup. Note the 8.3 names — `sh.mod`
goes on the card as `SH.MOD`.

**Resident in flash.** Concatenate the modules into an image and place it in the
module region, `0x10100000`–`0x11000000`. There is no directory there: the
kernel searches for the sync word, exactly as OS-9 did with ROM — the module
*is* its own directory entry.

```bash
python3 make_flash_image.py build/modules.bin \
        build/sh.mod build/ls.mod build/cat.mod build/echo.mod \
        build/lsmod.mod build/free.mod build/termdesc.mod build/usbdesc.mod
picotool load build/modules.bin -t bin -o 0x10100000
```

The filename comes before `-t` and `-o`; picotool rejects them the other way
round.

Flash is searched before the card. A module of the same name on the card is
registered alongside it, and whichever was registered first wins the lookup.

## Devices

A device is added by dropping a descriptor into the system, not by rebuilding
the kernel. The descriptor is a data module — no code, no entry point — stating
what the device is called, which driver handles it, and carrying a tail that
only that driver understands. To change UART or baud rate you change the
descriptor.

| Descriptor | Device | Driver        |
|------------|--------|---------------|
| `termdesc` | `term` | `UART    MOD` |
| `usbdesc`  | `usb`  | `USBCDC  MOD` |

If no descriptors are found at all, the kernel registers a built-in UART
console, so the system never goes mute.

## Startup

1. Scheduler, I/O manager and module directory are initialised
2. Flash is scanned for resident modules
3. The SD card is mounted and `.MOD` files are registered
4. Data modules are read as device descriptors and the devices are created
5. If `sh` exists only that is started, with 0/1/2 opened on `usb`, else `term`
6. With no shell, everything found is started, so the system still shows signs of life
7. The timer starts and pre-emption begins

## The console

The board appears as a USB serial port:

```bash
screen /dev/cu.usbmodem0000011 115200
```

The device name differs between boards; `ls /dev/cu.usbmodem*` shows which.

A UART console runs in parallel on **GP44** (marked A4 on the header) at 115200
baud, but output only — the descriptor sets `rx_pin` to `0xffffffff`, so it
accepts no input. Kernel startup messages can be followed there, but the shell
can only be driven over USB.

## The shell

```
myrtos> help
Type a module name to run it. Built in:
  help   this text
  lsmod  list modules
  free   memory and processes
  echo   print its arguments
  ls     list the SD card
  cat    show a file
  cp     copy a file
  rm     delete a file
  write  write text to a file
  sleep  wait, in milliseconds

A trailing & runs a command without waiting for it.
```

`help` is the only thing the shell does itself. Everything else is a module
looked up in the directory and started — `lsmod`, `free`, `echo`, `counter`,
`cxxdemo`, `usbecho`. The shell opens nothing; it inherited 0, 1 and 2 from the
kernel, and everything it starts inherits them in turn, so a utility neither
opens nor knows about any device.

## System calls

`a7` carries the number, `a0`–`a2` the arguments, `a0` comes back with the
result. Inline wrappers for all of them are in the ABI header.

| No | Name | Arguments |
|----|------|-----------|
| 0 | `SYS_NULL` | — |
| 1 | `SYS_IO_PUTC` | character |
| 2 | `SYS_EXIT` | — |
| 3 | `SYS_OPEN` | device name → path number |
| 4 | `SYS_WRITE` | path, buffer, length |
| 5 | `SYS_CLOSE` | path |
| 6 | `SYS_MODDIR` | index, buffer → link count |
| 7 | `SYS_MEMINFO` | what → value |
| 8 | `SYS_READ` | path, buffer, length → bytes read |
| 9 | `SYS_EXEC` | module name, arguments → pid |
| 10 | `SYS_ARGS` | buffer, length → characters copied |
| 11 | `SYS_FSDIR` | index, name buffer, &size → attributes |
| 12 | `SYS_FSREAD` | &request → bytes read |
| 13 | `SYS_FSWRITE` | &request → bytes written |
| 14 | `SYS_FSREMOVE` | name → 0 or -1 |
| 15 | `SYS_WAIT` | pid; returns when it has exited |
| 16 | `SYS_SLEEP` | milliseconds; returns when they have passed |

The trap vector hooks the SDK's weak vector symbols instead of owning `mtvec`
itself. That was not the first attempt: taking `mtvec` worked until TinyUSB was
initialised and hard-asserted in `irq_add_shared_handler`. Hooking in rather
than fighting removed more code than it added.

## Waiting

A process that waits is taken off the run queue. Two things can be waited for,
and they are woken differently.

A **read** with nothing to return steps `mepc` back onto the `ecall` and blocks.
When the process runs again it re-executes the call with its arguments still in
place, so the kernel remembers nothing about a half-finished read. The wake-up
is a check once per timer tick, against a `readable` the driver provides. A
driver that cannot answer is never waited on -- the send-only UART would
otherwise park a shell on input that cannot arrive.

**Waiting for a process** is exact: the wake-up is a line in the exit path, and
nothing is polled to notice it. The shell uses it, which is why a command's
output appears before the next prompt.

**Sleeping for a length of time** uses a delta list, as in Comer's XINU. Each
sleeper stores not when it wakes but how many ticks after the one ahead of it,
so the timer decrements exactly one number per tick however many are asleep.
Absolute wake times would mean comparing every sleeper against the clock on
every tick, and would need an answer for what happens when the clock wraps. The
cost moves to insertion, which walks the list summing deltas -- a process sleeps
once and is ticked many times, so that is the right way round.

Neither existed at first, and the cost was visible: `fill big.txt 100000` took
26.6 seconds while the shell polled for input beside it, and 20.5 seconds once
the shell was asleep.

## What is missing

- **No writing to the filesystem.** FAT32 support is read-only, so `rm`, `cp`
  and `mkdir` cannot be written yet.
- **No protection.** No MPU, no user mode — a module can write anywhere.
  Position independence is what makes sharing possible, not a guard rail.
