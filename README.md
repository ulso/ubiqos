# UbiqOS

A real-time operating system for the RP2350: programs are
**position-independent modules**, one copy of the code is shared by every
process running it, and a module runs where it lies -- straight out of flash,
or from wherever it was loaded off the SD card.

It runs on the Cortex-M33 and on the Hazard3 RISC-V core, with a shell on an
HDMI screen and a USB keyboard, a network over the USB cable and over WiFi, and
modules written in C, C++, D, Zig or Rust -- or compiled to WebAssembly.

## Boards

| Board | What UbiqOS uses |
|---|---|
| **Adafruit Fruit Jam** (RP2350B) | HDMI console, USB keyboard through the on-board hub, microSD over 4-bit SDIO, 8 MB PSRAM, the TLV320 audio codec, WiFi through the ESP32-C6, NeoPixels and buttons |
| **Waveshare RP2350-Touch-LCD-4.3B** | the 800x480 RGB panel, the GT911 touch controller, microSD, PSRAM; the USB-C port as device or as host |

## What exists

- **Kernel**: pre-emptive scheduling on both architectures, message passing with
  QNX's semantics, a TLSF heap in SRAM and a second pool in PSRAM, owners
  recorded so a process that dies returns what it held
- **Modules**: resident in flash or loaded from FAT32 on the card, a module
  directory with link counts and revisions, a relocating loader, and driver
  modules that bring their own interrupt handlers
- **Files**: FAT32 with long file names, volumes (`/sd`, `/dev`), POSIX-style
  descriptors, and the card offered to a computer as a USB disk
- **Console**: UTF-8 with ANSI escape sequences on HDMI, through a character
  generator or a framebuffer, and on USB CDC at the same time
- **Shell**: arguments, pipes, redirection with `>` and `>>`, and `&`
- **Network**: lwIP over USB CDC-NCM -- the board is 192.168.7.1 on the cable
  and hands the computer an address by DHCP -- and over WiFi through Espressif's
  ESP-Hosted; mDNS, SNTP, `ping`, a web server, `httpd`, and a TLS client:
  `fetch https://...` checks the server's certificate as a browser does
- **Programs**: about ninety modules, among them an editor (Atto emacs), a
  WAV player, `hibouair` for BLE air-quality sensors through a BleuIO dongle,
  and the usual `ls`, `cat`, `cp`, `mv`, `rm`, `ps`, `kill`
- **An SDK** for building a module, or a whole application image, outside
  this tree

## Getting started on a Fruit Jam

1. Hold **button 1** (BOOT), press and release **reset**, and let go of button 1
   when a drive called `RP2350` appears.
2. Copy `ubiqos.uf2` onto it, and press **reset** when the copy is done: the
   board may stay in the bootloader rather than restart by itself. Copy the
   file rather than using `picotool load`, which writes only the first of the
   file's two block families and leaves the old system in place.
3. Talk to it on the HDMI screen with a USB keyboard in one of the host ports,
   or over the USB-C cable: `screen /dev/cu.usbmodem… 115200` on a Mac, PuTTY
   on the COM port on Windows.
4. The cable is also a network: `ping ubiqos.local`.

WiFi needs the ESP32-C6 reflashed with ESP-Hosted once, and a `config.txt` on
the card naming the network. [docs/fruit-jam.md](docs/fruit-jam.md) goes
through all of it: the card, the WiFi firmware, `config.txt`, the web server,
the sensors, and having them start by themselves.

## Building

You need the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1, CMake,
Ninja, Python 3, and the Arm toolchain -- plus the RISC-V toolchain for a RISC-V
build. The installer of the Pico VS Code extension puts all of them under
`~/.pico-sdk`.

```bash
git clone --recursive https://github.com/ulso/ubiqos.git
cd ubiqos
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.1
cmake -S . -B build -G Ninja
ninja -C build
```

That is the Fruit Jam on Arm with the character console, and the result is
`build/ubiqos.uf2`: the kernel and every resident module in one file. The other
configurations are chosen when configuring:

| Configuration | Add to `cmake` |
|---|---|
| Fruit Jam, Arm, character console | (the default) |
| Fruit Jam, Arm, framebuffer | `-DUBIQOS_VIDEO=framebuffer` |
| Fruit Jam, RISC-V | `-DUBIQOS_ARCH=riscv -DPICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_3_0_0` |
| Waveshare 4.3B | `-DUBIQOS_BOARD=ws43b` |
| Waveshare 4.3B, USB-C as host | `-DUBIQOS_BOARD=ws43b -DUBIQOS_NATIVE_USB=host` |

**The framebuffer builds do not work at present**, on Arm or RISC-V: they boot
to a shell on the HDMI screen, but the 300 kB framebuffer leaves too little
SRAM for the rest, so there is no USB console, no SD card and no WiFi. They
still build, and are left out of releases until that is fixed.

Use a separate build directory for each. Only Release builds fit in RAM, and
that is forced. clang, `ldc2`, `zig` and `rustc` are used for modules in those
languages when they are found, and skipped when they are not.

## Documentation

| | |
|---|---|
| [docs/fruit-jam.md](docs/fruit-jam.md) | setting up a Fruit Jam, step by step, as far as WiFi and the sensor page |
| [docs/design.md](docs/design.md) | the long account: modules, display, console, shell, system calls, audio, pins, interrupts, scheduling |
| [docs/writing-modules.md](docs/writing-modules.md) | writing a module |
| [docs/the-sdk.md](docs/the-sdk.md) | building a module or an application outside this tree |
| [docs/config.md](docs/config.md) | `/sd/config.txt`: name, WiFi, time zone, the cable's address |
| [docs/esp-hosted](docs/esp-hosted/README.md) | the WiFi co-processor and how its firmware gets there |
| [docs/tls.md](docs/tls.md) | https: what is checked, the roots, and using it from a module |
| [docs/usb-sleep.md](docs/usb-sleep.md) | the USB network after the computer sleeps |
| [docs/the-board-stopped.md](docs/the-board-stopped.md) | a hang with the picture still on the screen, and what it was |

## What it is not

There is no memory protection: no MPU, no user mode, and any module can write
anywhere. Position independence is what makes sharing code possible, not a
guard rail.

## Licence

UbiqOS is released under the [MIT License](LICENSE). The third-party code it
includes keeps its own licence, as below.

## Third-party code

UbiqOS is built on the Pico SDK, TinyUSB, Pico-PIO-USB, lwIP, Mbed TLS, wasm3,
Atto and the Terminus font. What each one is, where it lives and its licence
are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), which should travel
with any UF2 passed on.
