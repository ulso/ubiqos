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

**Release, and not by preference.** A debug build wants about eight kilobytes
more RAM than the machine has — the framebuffer is 307200 bytes of a 520 kB part
and the kernel is linked `copy_to_ram`, so `-Og -g` does not fit. The failure is

```
region `RAM' overflowed by 8132 bytes
```

at the link, with nothing to say the build type was the cause. `.vscode` pins
`CMAKE_BUILD_TYPE` to Release for that reason; from the command line, pass it or
let it default.

The toolchain above is the one the SDK 2.2.0 installer left behind, and it
works. SDK 2.3.0 prefers `gcc-riscv32-pico-elf`, which can target the core the
board actually has with `-mcpu=hazard3-rp2350`; that is not in use yet.

The exports matter every time, not just the first. `pico_sdk_import.cmake` reads
the environment **only when the cache has no value**:

```cmake
if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
```

So a cache that once got the wrong path keeps it, and a later `cmake -S . -B
build` faithfully preserves it. VS Code re-running configure — after an
extension update, say — is enough to put one there. The symptom is

```
rp2350-riscv.cmake does not exist. Either specify a valid PICO_PLATFORM
```

which reads like a broken SDK and is a stale cache. Delete
`build/CMakeCache.txt` and configure again with the exports set. Nothing is lost:
`build` is not in the repository, and everything in `.vscode` is.

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

A module need not be in this tree. [`docs/the-sdk.md`](docs/the-sdk.md) is how a
separate repository builds one against the same knowledge, and
[`sdk/example`](sdk/example) is a working application that does.

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

The build also produces `myrtos.uf2`, which is the kernel and that image in one
file -- two addresses a megabyte apart, which one UF2 can hold because every
block carries its own target address. There is a second module region at
`0x10800000` for an application built elsewhere; `tools/combine_uf2.py` folds
its image in beside this one, or ships it alone when only it has changed.

**From the SD card.** Copy the `.mod` files to the root of the card. The
filesystem is FAT32, read and write, and the 8.3 names are uppercase —
`dhello.mod` goes on the card as `DHELLO.MOD`.

Nothing is read off the card until something runs it. A name that is not in the
module directory and not in flash is looked for on the card, read in, and
registered; when the last process using it exits, the entry goes and its memory
is freed with it. A module occupies memory for exactly as long as something is
using it. Flash modules work the same way and always have: they are never
copied at all, since the code runs where it lies.

Flash is searched first, so a module that exists in both places comes from
flash and the card copy is never read. Take it out of `MYRTOS_RESIDENT` if the
card version is the one being worked on.

### Getting files onto the card without moving it

`usbdisk` hands the card to the host over USB, so a program built on the Mac
can be copied straight onto it.

```bash
usbdisk                              # on the board: the card appears as MYRTOS
```
```bash
cp build/dhello.mod /Volumes/MYRTOS/DHELLO.MOD    # on the host
diskutil eject /Volumes/MYRTOS                    # or eject in Finder
```
```bash
usbdisk off                          # on the board: take it back
```

**Eject on the host before `usbdisk off`, and do not skip it.** The eject is
what makes the host flush what it is still holding; pulling the card out from
under it can leave a directory update half written. And `usbdisk off` is needed
*after* the eject because macOS unmounts a volume and stops asking without
sending `START_STOP_UNIT` — the board never hears that the eject happened.

Only one side may have the card at a time, which is why `usbdisk` unmounts
`/sd` first and `mount` refuses while the host still has it. Both filesystems
cache the directory and the free-cluster map; if both write, the volume is
ruined in seconds and neither notices until it reads something back.

Taking the card back is not a mount and does not need one: it was lent, not
lost, so it is still on whatever bus it was using, and only the filesystem is
re-read.

**If the host is a Mac, do this once**, and mounting goes from fifteen seconds
to none:

```bash
touch /Volumes/MYRTOS/.metadata_never_index
mkdir -p /Volumes/MYRTOS/.fseventsd && touch /Volumes/MYRTOS/.fseventsd/no_log
```

Measured: the first mount of a card without those read 15920 sectors — eight
megabytes off a volume holding a hundred kilobytes of files — because Spotlight
indexes anything it is shown. With indexing off, the same mount reads 91
sectors. `usbdisk off` prints what the host read and wrote, so this is visible
rather than folklore.

## Devices

A device is added by dropping a descriptor into the system, not by rebuilding
the kernel. The descriptor is a data module — no code, no entry point — stating
what the device is called, which driver handles it, and carrying a tail that
only that driver understands. To change UART or baud rate you change the
descriptor.

| Descriptor | Device | Driver        | What it is                            |
|------------|--------|---------------|---------------------------------------|
| `termdesc` | `term` | `UART    MOD` | the UART on GP44, send only           |
| `usbdesc`  | `usb`  | `USBCDC  MOD` | the serial port to the host           |
| `kbddesc`  | `kbd`  | `KEYBRD  MOD` | the USB keyboard, and the layout      |
| `condesc`  | `con`  | `CONSOLE MOD` | the screen and the keyboard together  |
| `acmdesc`  | `acm`  | `ACM     MOD` | a CDC-ACM device in the USB socket    |

`acm` is the serial port at the other end of the USB socket rather than at the
other end of the cable to the Mac — a BLE dongle, a modem, a sensor. It is a
character device like the others, so `cu acm` is all it takes:

```
myrtos:/> cu acm ATI
connected; ctrl-C to stop
ATI
Smart Sensor Devices AB
DA14695
BleuIO Pro
Firmware Version: 1.0.5.6
```

Anything after the device name is sent as one line first, which saves typing a
command that is always the same. `ctrl-C` stops it — `cu` has no escape
sequence of its own and does not need one.

One warning that cost an afternoon. TinyUSB leaves DTR and RTS low unless
`CFG_TUH_CDC_LINE_CONTROL_ON_ENUM` says otherwise, and a CDC-ACM device reads
DTR as "the host has opened the port": ours echoed everything typed at it and
answered nothing. Write that constant as a **number**. The class driver guards
it with `#if`, and `CDC_CONTROL_LINE_STATE_DTR` is an enum rather than a macro
— so the preprocessor reads it as an undefined name, evaluates it to zero, and
compiles the request away. Symbolic, readable, and silently nothing.

Nothing is buffered on our side. TinyUSB keeps a packet each way and the USB
thread empties it every millisecond, which is quicker than a shell reads.

If no descriptors are found at all, the kernel registers a built-in UART
console, so the system never goes mute.

## Startup

1. Scheduler, I/O manager and module directory are initialised
2. Flash is scanned for resident modules
3. Data modules are read as device descriptors and the devices are created
4. If `sh` exists only that is started, with 0/1/2 opened on `usb`, else `term`
5. With no shell, everything found is started, so the system still shows signs of life
6. The timer starts and pre-emption begins
7. The filesystem server, now a running process, mounts the SD card — four-bit
   SDIO first and SPI if that fails — and runs `/sd/startup` if it is there

The card is deliberately last, and in a process rather than here. Mounting it
before the scheduler starts means a driver that stalls takes the console and USB
with it, and the only way back is the BOOTSEL button; in a process a stall costs
one process. Nothing the machine needs to boot lives on the card — the shell and
every descriptor are resident in flash.

`/sd/startup` is run by an ordinary shell with its standard input pointed at the
file, so it is a list of commands and nothing more. It is started, not waited
for: the system is up either way.

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

A program changes colour by writing the codes; there is nothing to open and no
call to make, and everything written after one comes out in the new colour.
The ABI has `myrtos_line_colour` for building the escape into the same
`myrtos_line_t` as the text, which matters for the same reason the line buffer
exists at all: a write is atomic and a pair of them is not, so a colour set in
one write and the text printed in the next colours whatever another process
printed in between. `color` is the command, and its source is the example.

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
  mount    which bus the card is on, or 'mount sdio' to take it again
  wifi     firmware, or 'wifi scan' for networks
  font     screen font, or 'font 6x12' to change it
  cat      show a file
  cp       copy a file
  rm       delete a file
  mv       rename a file, or move it within a volume
  write    write text to a file
  sleep    wait, in milliseconds
  nice     run a command at a priority
  bootsel  reboot into the bootloader

A trailing & runs a command without waiting for it.
Arrows move along the line and up and down the history; ctrl-A and
ctrl-E jump to its ends; ctrl-L clears the screen when it is empty.
ctrl-C ends the running command, or abandons the line if none is.
```

Ctrl-C is caught in the driver, where the byte arrives, and never becomes data
while a command is running. It has to be: the process it is meant for is usually
blocked in a rendezvous reading nothing at all — `wifi scan` sits in
`WAIT_REPLY` for eight seconds — and the only thing reading the keyboard at that
moment is the *other* shell. The kernel cannot tell which process was meant, so
the shell says: it names its foreground process with `SYS_FOREGRND` around every
command it waits for.

On the serial port the key is found by peeking at the head of the CDC FIFO in
the USB task, so it is seen when it is the next byte — which it is unless
something was typed first and left unread.

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
| 40 | `SYS_KILL` | pid → 0, or -1 if there is no such process or it is refused |
| 41 | `SYS_FOREGRND` | path, pid or 0 → 0, or -1 if there is no such path |
| 42 | `SYS_WIFIJOIN` | "ssid\0pass" → 0 joined, otherwise the status |
| 43 | `SYS_WIFIADDR` | buffer, length → 0 if there is an address |
| 44 | `SYS_RECEIVETMO` | &message out, milliseconds |
| 45 | `SYS_SEEK` | descriptor, offset, whence |
| 46 | `SYS_FSSTAT` | &stat → attributes, or -1 |
| 47 | `SYS_DUP` | descriptor, new one or -1 → the new one |
| 48 | `SYS_PIPE` | &fds[2] → 0, or -1 if none can be had |
| 49 | `SYS_LOADMOD` | module name → 0, or -1 if not on the card or no room |
| 50 | `SYS_REBOOT` | starts the machine again; never returns |
| 51 | `SYS_USBDISK` | 1 hands the card to the host, 0 takes it back |
| 52 | `SYS_PULSE` | pid, type, value → 0, or -1; never blocks |
| 53 | `SYS_ARM` | path, pulse type, 0 cancels → 0, or -1 if there is no room |
| 54 | `SYS_DISARM` | → how many watches this process held |
| 55 | `SYS_USBINFO` | which field → that field of the USB host state |
| 56 | `SYS_RANDOM` | buffer, length → bytes filled |
| 57 | `SYS_CATCHINTR` | pulse type, 0 to go back to being killed |
| 58 | `SYS_WIFISOCK` | op, port or socket, &{buffer,length} |
| 59 | `SYS_FSRENAME` | &rename → 0, or -1 |
| 60 | `SYS_WIFIRESET` | — ; the chip comes back on no network |
| 61 | `SYS_GETSTAT` | path, code, &{data,length} → 0, or -1 |
| 62 | `SYS_SETSTAT` | path, code, &{data,length} → 0, or -1 |

## Playing a WAV file

`play FILE`, and `play -i FILE` to see what a file is without playing it. PCM
only, 8, 16, 24 or 32 bits, integer or IEEE float, mono or stereo, **any rate**.

The device runs at 48000 and only 48000, because that is what an integer PIO
divider gives from a 120 MHz system clock -- and most of the free WAV files on
the internet are 44100. So the player resamples, and it asks the device what
rate to resample *to* rather than having 48000 written in a second time.

One method, a polyphase FIR: a Kaiser-windowed sinc, ten zero crossings each
side, sampled 64 times between each pair with the fraction interpolated between
those. `tools/make_sinc.py` generates the coefficients, because a module has no
floating point to compute them with and no libm to ask for a sine.

The filter stretches with the ratio, and that is the whole trick. Upward it is
a fixed 20 taps. Downward the impulse response is scaled by the ratio, which
moves the cutoff from the input Nyquist down to the **output** Nyquist, and so
stops content above it folding back into the band. Measured, against the linear
interpolation and box average this replaced:

| | before | after |
|---|---|---|
| 440 Hz, 44100 → 48000 | -90 dB | **-109 dB** |
| 10 kHz, 44100 → 48000 | -72 dB | **-103 dB** |
| a 30 kHz tone folding down, 96000 → 48000 | -5 dB | **-100 dB** |

### The clock the codec is given

The codec runs off a **master clock on GP25**, 48 MHz out of `clk_usb` divided
by one, with its own PLL powered down. `NDAC=2, MDAC=4, DOSR=128` divides that
to exactly 46875 Hz.

It did not always. The PLL used to lock to the bit clock, because MCLK costs a
pin and so nobody wires it -- Adafruit's CircuitPython does the same on this
board, and **both distorted identically**: a 10 kHz tone came out with
sidebands 16 dB down, and with a 2.35 kHz component that was most of what
anybody actually heard. Two independent stacks failing the same way is what
pointed at the one thing they had in common.

| | codec PLL on BCLK | MCLK, PLL off |
|---|---|---|
| sideband at 11314 Hz | -16.0 dB | **-59.2 dB** |
| sideband at 17822 Hz | -19.6 dB | **-54.9 dB** |
| audible junk under 9 kHz | -25.7 dB at 2350 Hz | nothing above the mains hum |

**46875 and not 48000**, because 48 kHz with `DOSR=128` needs a 49.152 MHz
master clock, and that family cannot be divided out of a 12 MHz crystal at all.
That is precisely why the board leaves MCLK unconnected in the first place.
Nothing has to care: `/dev/audio` answers `MYRTOS_SS_RATE` and `play` resamples
to whatever it says. The PIO divider comes out at exactly 40 as well.

**DOSR must be a multiple of eight.** Interpolation filter A upsamples by eight
before the rest of the oversampling. A first attempt used `DOSR=100` to hit
48000 exactly from 48 MHz, and put a comb of 2 kHz sidebands around every tone
-- worse than the fault it was fixing.

`play -v` times a playback against the board's own clock`play -v` times a playback against the board's own clock and prints what the
work cost against what the audio is worth. Everything is at or under real time
except 32-bit float, which is 36% over and stutters; the cause is recorded in
the source and is not the arithmetic.

The 32-bit float case is decoded by taking the exponent and mantissa apart with
integer shifts. A module is linked without libgcc, so on the RISC-V half of
this system -- no hardware FPU -- touching a float at all is a call to a
soft-float routine that is not there.

## Who owns which pin

`gpio` lists all 48 of them with their level and their owner, and that listing
is the useful part. This board fixes most of its pins in hardware -- eight for
video, seven for the SD card, six for the WiFi, three for the USB host -- and
the drivers take several more. "Which pins can I actually use" has a short
answer: **GP6 to GP10** on the socket header, GP45, GP47, and whichever of
GP40-43 the ADC is not using.

`kernel/pins.c` holds one table with one owner per pin. What the board fixes is
claimed at boot; what a driver uses is claimed by that driver when it
configures itself, so the table says what is true of this boot rather than what
is true of the board.

```
myrtos:/> gpio 44 out
gpio: GP44 belongs to term
myrtos:/> gpio 24 out
gpio: GP24 belongs to audio
```

That is the whole point. GP44 is A4 on the header and looks like a free
analogue pin; it is the terminal. Driving it would have taken the console away
and the machine would have gone quiet with nothing to explain it.

It works in both directions. Take GP40 with `gpio` and the ADC is refused when
it is started, with a message saying to go and look:

```
myrtos:/> gpio 40 out
myrtos:/> adc 1
adc: refused -- one of GP40-43 belongs to something else. Try 'gpio'.
```

### Waiting for a button

```
myrtos:/> gpio 4 up
myrtos:/> gpio watch 4
watching GP4, ctrl-C to stop
GP4 pressed at 102756 ms
GP4 released at 104050 ms
```

`gpio watch PIN [DEBOUNCE_MS]` **blocks**. It does not poll: the driver's
`readable` tells the scheduler whether an event is waiting, the scheduler runs
the process again when one is, and in between it uses no time at all. The
handler never wakes anybody -- it may not, and it does not need to.

**The handler is at 0x80, not 0x40.** The ADC's outranks the kernel because it
has a deadline; a button does not. At the ordinary peripheral level a kernel
critical section does hold this off, which is right: a few microseconds late to
a button press is not measurable, and the rules that come with outranking the
kernel are not worth taking on for nothing.

Debouncing is in the handler, 20 ms by default. Measured on this board it never
fires -- eight presses gave sixteen interrupts and sixteen events, so the
switches do not bounce measurably and there is presumably an RC filter on them.
The mechanism is there for a switch that does.

Ownership and what a pin is *called* are different questions. The buttons own
nothing -- they are three switches on three pins, and a program that wants one
should have it -- so they are not in the board's claim table. What they are
called lives in the driver, and the listing shows it in brackets:

```
GP 4  1  (button2)     free, and named
GP44  1  term          owned
```

**It is not enforcement.** Nothing stops a driver writing to a pin it never
claimed -- that wants the memory protection unit and a great deal more. What it
buys is that the question can be asked and answered.

A driver that finds a pin taken **returns a failure rather than printing a
warning**. A `print` from inside a system call does not reach the console: the
message queues behind a USB task that cannot run until the trap returns, and it
is simply lost. The ADC did that first, and went on to configure a pad it did
not own, silently -- which is the one thing the registry exists to stop.

## An interrupt that belongs to a driver

`irq_install` in the kernel API lets a driver module take an interrupt at a
priority of its choosing. The analogue inputs are the first thing to use it:
`/dev/adc` runs its handler at **0x40** against `kernel/critical.h`'s threshold
of **0x80**, so a kernel critical section never masks it.

Measured, with `crit` holding a critical section for 500 microseconds twenty
times over and `adc -i` reporting the worst gap between two runs of a handler
that the hardware asks for every 125 microseconds:

| kernel masks with | worst gap | conversions missed |
|---|---|---|
| `PRIMASK` (the default) | 555 us | 20 |
| `BASEPRI` 0x80 (`-DMYRTOS_BASEPRI=0x80`) | **127 us** | **0** |

**Confirmed from outside.** `adc -p 6` gives the handler a pin to toggle, so an
oscilloscope on GP6 sees a square wave whose half-period is the handler's
interval. Under 400 deliberate 500 microsecond critical sections, three
captures of 2000 edges each: median 124 us, maximum 128, and not one gap over
1.5x the median. The number the driver reports is the handler measuring itself;
this is a different instrument answering the same question.

The handler writes the SIO register directly rather than calling `gpio_put`
through the kernel API table. Going through that table is what the rule above
forbids; writing this driver's own bit in the SIO is its own hardware, which is
what the rule allows.

**And the sharper argument for BASEPRI is not the latency.** The same 400-hold
burst under `PRIMASK` takes USB down permanently -- 200 milliseconds of masking
in total, in 500 microsecond pieces, is enough for the host to give up on the
device, and it does not come back without the BOOTSEL button. Under BASEPRI the
same burst does not disturb it at all, because USB is at 0x80 and only the
kernel's own work is held off.

The test had to make its own critical section. Real ones in this kernel measure
under three microseconds -- `tlsftest`, a hundred-kilobyte file read and a WAV
playback all leave the worst gap at 127 to 128 us -- so against real work the
two mechanisms are indistinguishable, and an experiment that waited for one to
happen would have said nothing.

### What a handler above the kernel may not do

**A handler more urgent than the threshold may not call anything in this system.
Not one function. That is the price of never being masked, and it is not
negotiable.**

Concretely, from such a handler:

- **No system calls.** Not `myrtos_write`, not `myrtos_read`, not `myrtos_open`,
  not `myrtos_sleep`. A system call is a trap, and traps here are how the kernel
  is entered from a thread -- taking one from inside an interrupt is not a
  smaller version of that, it is a different thing entirely.
- **No messages.** `myrtos_send`, `myrtos_receive`, `myrtos_reply` and
  `myrtos_pulse` all walk the process table.
- **No allocation.** `mem_alloc`, `driver_alloc` and `free` walk the allocator's
  own structures, which is very likely what the kernel was doing when it was
  interrupted.
- **Nothing through the kernel API table.** `print` included: it reaches the
  console driver and the I/O manager.
- **No printing at all**, which is the one people reach for while debugging and
  the one that will corrupt the thing being debugged.

What a handler may do is its own hardware, its own memory, and nothing else.
The ADC driver's handler is the whole shape of it: read the FIFO, write two
arrays this driver owns, return. A reader picks those arrays up later, in
ordinary thread context, where every one of the above is allowed again.

**Why the rule exists.** The kernel protects its data with critical sections,
and a critical section is exactly what a handler above the threshold ignores.
So such a handler runs while the process table, the path table or the
allocator's free lists are half-updated. Touching them then corrupts them, and
the corruption surfaces somewhere else entirely, minutes later, looking like
anything but its cause. FreeRTOS calls the same threshold
`configMAX_SYSCALL_INTERRUPT_PRIORITY` and draws the same line for the same
reason.

**Handing work out.** Something lock-free, and simple enough to be obviously
so. A single aligned 32-bit store is atomic on both machines here, which is
enough for a latest-value or a counter -- that is all the ADC driver needs. A
ring wants a little more care and no locks either. If a handler seems to need a
lock, it is at the wrong priority: put it at 0x80 with everything else, where a
critical section does hold it off and the rules are the ordinary ones.

A driver may also be brought up a step at a time -- `adc 1` through `adc 4` --
which is not fussiness. A driver that takes an interrupt at boot and gets it
wrong takes the machine down before USB is up, and the only way back in is the
BOOTSEL button.

## Talking to the WiFi coprocessor

A command is a **transaction**: write the frame, read the whole reply into a
buffer, then decide. Every check is against the buffer and never against the
wire, every failure lands on one `resync()`, and a failed transaction is sent
again exactly once.

That shape is the answer to a measured fault. The channel would go one step out
of step and stay there -- the chip demonstrably on the network and answering
ICMP while every command over SPI failed, `GET_FW_VERSION` included, which
shares nothing with the socket commands but the framing. The old code decided
byte by byte while the chip was still talking; one wrong branch left the rest of
a reply unread, the next command read that tail as its own answer, and every
command after it was answering the one before. It ran perfectly until the first
glitch and was gone from then on.

`wifi stats` says how it has actually been going:

```
commands  2
resyncs   0
retries   0
failures  0
```

The middle two are the point. A channel that resynchronises occasionally and
one that resynchronises constantly look identical from outside, and telling
them apart is the whole question -- a day went on the wrong half of it for want
of a count. It also separates "the chip answered, and the answer was no" from
"the command did not get through", which is a distinction the old code could
not make: `wifi ip` with no network now reports no address with **no** failures.

**Retries are opt-in, and off for sends.** A query may be asked twice: it costs
a round trip and tells the truth either way. A send may not. A transaction fails
when the *reply* did not parse, and the chip may perfectly well have taken the
data and sent it -- so asking again puts the same bytes on the wire twice, and a
page that arrives corrupted is worse than one that does not arrive, because the
far end cannot tell.

Measured with the BLE scanner running, which used to kill the link after two
fetches:

| | |
|---|---|
| `/api/sensors`, 40 rounds | 40 of 40, every one 671 bytes |
| the 4 kB page, 40 rounds | 39 full, one empty, and it recovered |
| the command channel over both | 580 commands, **0** resyncs, retries, failures |

**And then the receive, which is where the real bug was.** The reply's length
field in `getDataBufTcp` is **big-endian** -- nina-fw builds that one by hand
where every other length in the protocol is a `memcpy` out of a `uint16` on a
little-endian chip -- and this file read it the other way round from the day it
was written. A 255-byte reply became a claim of 65280, was capped to the
caller's buffer, and the extra reads got the chip's `0xff` idle filler. A web
server finds its request in the first part and ignores the tail, so it worked,
and the frame was never where the chip thought it was.

That is why adding a check for the frame's end marker broke every receive at
once: it was correct, and it was the first thing ever to look. Counting what
was actually in that position settled it in three minutes after two arguments
had produced two wrong answers:

| | end marker seen | data seen |
|---|---|---|
| before the fix | 0 of 100 | 100 of 100 |
| after | 120 of 120 | 0 |

With all three paths checked and counted, 120 requests with the BLE scanner
running: **120 of 120, and zero resyncs, retries and failures.** The load that
used to kill the link after two.

**DMA was the plan and the measurement said no.** The reason for DMA was
reliability, and reliability was fixed by the framing -- so it had to justify
itself on cost instead, and it could not. A small reply took 101 ms and a four
kilobyte page 164; the four kilobytes over SPI at 8 MHz are about **four**.
Under three per cent, and the big buffers live in PSRAM which DMA cannot reach,
so it would have wanted a bounce through SRAM to save a fraction of that.

The 101 ms was `httpd` sleeping 200 ms between asking whether anyone had
connected. That interval had a reason -- "each one is a chance for the protocol
to go wrong" -- and the reason had just stopped being true. Five milliseconds
for the first second after a request, then back to 200:

| | before | after |
|---|---|---|
| small reply | 101 ms | **46 ms** |
| 4 kB page | 164 ms | **46 ms** |

The page and the small reply now cost the same, which is the clearest statement
that the payload was never the expense. And the faster poll nearly tripled the
traffic to the chip -- 2682 commands in one run -- with **zero** resyncs,
retries and failures, and 667 receives all ending where they should.

**Connections are kept**, which is worth about a third of a page fetch to a
client that reuses one and costs eight milliseconds to a client that does not:

| | one connection each | one connection reused |
|---|---|---|
| before | 46 ms a page | -- |
| after | 54 ms | **29 ms** |

Every reply goes through one function with a real `Content-Length`, so a body is
always delimited -- which is the precondition, not a detail.

**Four connections at once**, which is what makes keeping them free. Before the
table, one client was served to the end before the next was looked at, so every
millisecond spent waiting for one client's follow-up was charged to whoever was
queued behind it -- the keep-alive wait had to be cut to fifteen milliseconds
for exactly that reason. Now a connection with nothing to say is skipped.

| 40 pages | |
|---|---|
| one client at a time | 2.3 s, 57 ms each |
| four clients at once | **1.2 s, 31 ms each** |

Not parallel: the wifi service handles one command at a time, so this
interleaves rather than overlaps. That is still the whole difference, because
what a client mostly does is think.

**A receive of nothing is not a closed connection.** It means nothing has
arrived yet, and a client that finished and went looks identical from here, so
the chip has to be asked -- once, after a tenth of a second of silence. Leaving
that out was worth measuring: every finished connection sat in the table until
it timed out, all four slots filled with the departed within a fifth of a
second, and a client opening a fresh connection each time went from 54
milliseconds a page to **484**.

**The rule, since it is not written anywhere in the protocol:** in nina-fw a
`memcpy` means host order -- the chip is little-endian -- and a hand-written
`>> 8` first means network order. The same file mixes them, with nothing at the
command level to say which. Every command this driver sends has been checked
against the firmware source; `getDataBufTcp`'s reply length was the only one
wrong. One other network-order field exists in the Arduino-style commands, the
port in `getRemoteData`, and we do not use it -- and the whole BSD-like socket
section is full of the same pattern. Adopt either and check the endianness of
every multi-byte reply field first.

Still on the old path: the scan and the connect, neither of which runs while a
page is being served. Then DMA, which is a small step once a frame is already a
buffer.

## Status: what a device is, rather than what it carries

`getstat` and `setstat`, from OS-9, and deliberately not Unix's `ioctl`. The
direction is which call you made rather than bits packed into the request
number, and the length travels beside the pointer, so a caller that thinks a
setting is a byte and a driver that thinks it is a word disagree once instead
of reading three bytes of somebody's stack ever after.

```c
uint32_t v = 40;
myrtos_setstat(fd, MYRTOS_SS_VOLUME, &v, sizeof v);
myrtos_getstat(fd, MYRTOS_SS_RATE, &v, sizeof v);   /* 48000 */
```

Codes below `0x100` mean the same on every device that answers them at all;
from `0x100` they belong to one kind of device. **An unknown code is always
-1**, and that is the discovery mechanism: a caller finds out what a device can
do by asking it, and a driver that has no settings implements neither entry.

The alternative is what this replaced. `volume` used to write the audio
codec's registers over `/dev/i2c`, so the chip's address, its two volume
registers and the fact that they hold signed half-decibels lived in a command
as well as in the driver -- and a second program could have changed the volume
without the driver ever knowing what it now was.


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

- **No protection.** No MPU, no user mode — a module can write anywhere.
  Position independence is what makes sharing possible, not a guard rail.
