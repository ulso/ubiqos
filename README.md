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

- Pre-emptive scheduling on the machine timer, 1 ms quantum, up to 32 processes
- A 64 kB TLSF heap in SRAM for what has timing constraints, which modules can
  ask from; the kernel records the owner, so death returns what death takes
- A second pool over the board's 8 MB of PSRAM for what is merely large
- Modules loaded from FAT32 on the SD card, or found resident in flash
- A module directory with link counts and revisions — a name exists once, and
  the highest revision of it wins
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

After the first time, the buttons are optional: `bootsel` at the shell hands the
board back to the ROM loader from software. `reset_usb_boot` is reached through
`rom_func_lookup`, which the SDK implements for RISC-V as well as Arm.

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

[`docs/writing-modules.md`](docs/writing-modules.md) covers what this rules out
in practice, which is less obvious than it sounds -- a switch returning string
literals breaks it, and so does the same code written as an if-chain.

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

**Resident in flash.** This is where the system lives. The build concatenates
`MYRTOS_RESIDENT` from `CMakeLists.txt` into an image and it goes into the
module region, `0x10100000`–`0x11000000`. There is no directory there: the
kernel searches for the sync word, exactly as OS-9 did with ROM — the module
*is* its own directory entry.

```bash
picotool load build/modules.bin -t bin -o 0x10100000
```

The filename comes before `-t` and `-o`; picotool rejects them the other way
round. The image is written whole, so a module is added by adding it to
`MYRTOS_RESIDENT` and loading the image again, never by loading one module.

**From the SD card.** Copy the `.mod` files to the root of the card. The
filesystem support is FAT32 and read-only; the kernel looks for files with the
extension `MOD` and registers them at startup. Note the 8.3 names — `sh.mod`
goes on the card as `SH.MOD`.

The two are not equivalent. A module in flash runs where it lies; one on the
card is read into RAM at startup and stays there for as long as the machine is
up, whether or not anything runs it. The card is for what is being worked on.

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

## The display

DVI out of the HSTX peripheral at 640x480, one byte per pixel, RGB332. The
peripheral's command expander does the TMDS encoding, so the processor never
touches a pixel: three DMA channels walk a table of scanline addresses into the
HSTX FIFO and there is no interrupt at all. The pixel clock is fixed at
`clk_hstx/5`, which is why the system runs at 125 MHz.

Scrolling rotates that table rather than moving pixels, so the framebuffer is a
ring and the console's origin is a line number in it.

Two fonts, both Terminus, both drawn by hand on their own grid rather than
scaled from each other:

```
myrtos> font
* 8x16, 80 by 30
  6x12, 106 by 40
myrtos> font 6x12
```

Switching clears the screen -- eighty columns do not reflow into a hundred and
six -- so what comes back is the next prompt.

### Why not a higher resolution

Memory, before anything else. A byte per pixel puts 640x480 at 307200 bytes of
the 520 kB of SRAM, and the kernel is linked `copy_to_ram`, so its code, its
64 kB heap and every process stack come out of what is left. 800x600 would want
480000, which does not fit at any clock. The glyph tables are the reason
`.flashdata` appears in the generated font files: six kilobytes that would
otherwise be copied into RAM for no reason, since nothing writes flash while
myrtos is running.

The way up is therefore fewer bits per pixel rather than more SRAM. The HSTX
expander takes each colour channel out of a bit field of a width it is told, and
the narrowest field it accepts is one bit, so a mono mode should be a matter of
the `EXPAND_TMDS` register and a packed framebuffer -- 800x600 in 60000 bytes,
which is less than the 640x480 costs now. A text console does not need 256
colours. This is read off the field widths and has not been tried; two things
would also have to be dealt with. The pixel clock becomes 40 MHz, so `clk_sys`
goes to 200 MHz. And the scanline table is currently 1024 entries aligned to its
own size, which is what makes the DMA ring work -- 600 active lines will not fit
in it and it would have to double.

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

## Escape sequences

The console understands enough ANSI to edit a command line on: `A`–`D` and `G`
and `H` to move the cursor, `J` and `K` to erase, `m` for colour, `s` and `u` to
save and restore where the cursor was, and `n` to say where it is. Anything else
is dropped rather than drawn.

Colour is the framebuffer's own RGB332. `30`–`37` and `40`–`47` are the eight,
`90`–`97` and `100`–`107` the bright ones; the bright eight are the values the
test card draws its bars from.

The keyboard sends the other half of the same language: the arrows, Home, End,
Delete and the page keys arrive as the sequences they have been since the
VT100, from `nav_sequence` in `kernel/usbhost.c`. They are not in the keymap on
purpose — a layout says which letter is on a key, and an arrow is an arrow
everywhere. The point is that the serial port and the screen deliver the same
bytes, so a program that reads a line needs one idea of how to edit it.

`ESC[999C ESC[6n` — move a long way right, then ask where you are — is how a
program finds the width of its terminal. Our console answers it by pushing the
report into the keyboard queue, because the console's input is the keyboard.

## The shell

```
myrtos> help
Type a module name to run it. Built in:
  help     this text
  lsmod    list modules
  ps       list processes
  keys     read the USB keyboard
  free     memory and processes
  echo     print its arguments
  ls       list a directory
  cd       change directory (built in)
  pwd      where you are
  mkdir    make a directory
  rmdir    remove an empty directory
  mount    take the SD card again
  wifi     firmware, or 'wifi scan' for networks
  font     screen font, or 'font 6x12' to change it
  cat      show a file
  cp       copy a file
  rm       delete a file
  write    write text to a file
  sleep    wait, in milliseconds
  nice     run a command at a priority
  bootsel  reboot into the bootloader

A trailing & runs a command without waiting for it.
Arrows move along the line and up and down the history; ctrl-A and
ctrl-E jump to its ends; ctrl-L clears the screen when it is empty.
```

The line editor keeps sixteen lines of history and refuses to take a character
that would push the line past the right edge — a wrapped line cannot be redrawn
from a carriage return, so it stops rather than draw it wrong. A long path
therefore wants a `cd` first, which is what one would do anyway.

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
| 6 | `SYS_MODDIR` | index, &info → 0, or -1 past the end |
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
| 17 | `SYS_SETPRIO` | new priority → the old one; zero asks without changing |
| 18 | `SYS_TICKS` | → milliseconds since the timer started |
| 19 | `SYS_PSINFO` | slot, &info → 0, or -1 for an empty slot |
| 20 | `SYS_BOOTSEL` | reboots into the bootloader; never returns |
| 21 | `SYS_ALLOC` | bytes → pointer |
| 22 | `SYS_FREE` | pointer → 0, or -1 if not ours |
| 23 | `SYS_REALLOC` | pointer, bytes → pointer |
| 24 | `SYS_DATAAREA` | &size → this process's data area and its size |
| 25 | `SYS_ALLOCBULK` | bytes → pointer, from PSRAM when there is any |
| 26 | `SYS_SEND` | pid, &message → the reply status |
| 27 | `SYS_RECEIVE` | &message out → the sender's pid |
| 28 | `SYS_REPLY` | status → 0, or -1 if nobody is being served |
| 29 | `SYS_PIDOF` | module name → pid, or -1 if not running |
| 30 | `SYS_MKDIR` | path → 0, or -1 |
| 31 | `SYS_CHDIR` | path → 0, or -1 if there is no such directory |
| 32 | `SYS_GETCWD` | buffer, length → characters copied |
| 33 | `SYS_RMDIR` | path → 0, or -1 if not there or not empty |
| 34 | `SYS_MOUNT` | — → 0, or -1 if there is no card |
| 35 | `SYS_REPLYTO` | pid, status → 0, or -1 if that one is not waiting on us |
| 36 | `SYS_WIFIVER` | buffer, length → 0, or -1 if the chip does not answer |
| 37 | `SYS_WIFISCAN` | -1 to look → count; an index → that network's signal |
| 38 | `SYS_CONFONT` | font or -1, &out, 1 to only look → 0, or -1 |
| 39 | `SYS_READABLE` | path → bytes waiting, 0 for none, -1 for no such path |

The trap vector hooks the SDK's weak vector symbols instead of owning `mtvec`
itself. That was not the first attempt: taking `mtvec` worked until TinyUSB was
initialised and hard-asserted in `irq_add_shared_handler`. Hooking in rather
than fighting removed more code than it added.

## Scheduling

Thirty-two priority levels, a ready queue for each, and a bitmap of which
queues are not empty. Choosing what runs next is finding the highest set bit,
which on this core is a single `clz` instruction, so the cost does not grow
with the number of runnable processes.

Round robin survives inside a level: a process that uses up its quantum goes to
the back of its own queue. Nothing is shared across levels, which is the point
of a priority scheduler and also its sharp edge -- a process that neither
blocks nor sleeps starves everything below it for as long as it runs.

A process's data area arrives in `tp`, so a module reaches its own state with
one instruction rather than a system call. The thread pointer is exactly the
right register for it: the data area is thread-local storage with our layout
instead of the compiler's, `.tdata` and `.tbss` are empty, and the trap frame
already saved and restored `tp` per process.

Priority is inherited across `exec`, as path numbers are, so `nice` needs no
help from the kernel: it sets its own priority and starts the command, which
knows nothing about any of it. The kernel creates the first process from the
idle level, so that one case takes the default instead of inheriting.

The idle process sits alone at the bottom, and never blocks. That is why
picking the next process needs no special case for "nobody is ready": the
bitmap is never empty.

**USB is serviced by a process of its own**, at priority 30. It was in the idle
process until priorities arrived, at which point anything busy above it
silenced the console in both directions -- received bytes reach TinyUSB's FIFO
only when `tud_task` runs, so even input stopped. One millisecond is far more
often than needed: CDC data has no deadline, and the tightest real limit is the
50 ms USB allows for answering a standard request with no data stage.

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

**Writing** blocks the same way when a device has no room. The USB console has
a 256-byte send buffer, and a write larger than that used to be cut off: the
driver called `tud_task` to drain it, from the trap handler, with interrupts
off -- so the transfer that would have drained it could never complete. That
was not merely truncation but a hang, and printing a long enough line stopped
the system. A write now takes what fits and reports how much, the caller loops,
and the kernel blocks it until there is room.

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
