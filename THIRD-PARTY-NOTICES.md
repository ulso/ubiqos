# Third-party notices

myrtos is built on other people's work. This file names each piece, says where
it is and under what terms, and carries the licence texts that must travel with
a binary. **Anyone passing on a myrtos UF2 should pass this file on with it** --
the BSD licences below ask for exactly that.

## In every UF2

| Component | Where | Licence |
|---|---|---|
| Raspberry Pi Pico SDK 2.3.1 | fetched by the build, not in this tree | BSD-3-Clause |
| TinyUSB, as shipped with the SDK | the SDK; two files patched in `lib/tinyusb-patched/` | MIT |
| Pico-PIO-USB | `lib/Pico-PIO-USB/` (git submodule) | MIT |
| lwIP 2.2 | the SDK's copy in the kernel, mDNS patched in `lib/lwip-patched/`; `third_party/lwip/` is a copy patched for the `lwipd` module | BSD-3-Clause |
| pico_sd_card, from pico-extras | `third_party/pico_sd_card/` | BSD-3-Clause |
| Terminus Font | `third_party/terminus/`; baked into `kernel/font6x12.c` and `font8x16.c` | SIL Open Font License 1.1 |
| wasm3 | `third_party/wasm3/`; the `wasm` module | MIT |
| Atto | `modules/atto/upstream/` | public domain |
| newlib-nano | the toolchain's C library, linked into modules that ask for it | BSD-style, see below |
| libgcc | the toolchain's runtime | GPL-3.0 with the GCC Runtime Library Exception |

The runtime library exception places no condition on a program built with GCC,
so libgcc needs no notice. newlib is a collection of many small licences, nearly
all BSD-style; the full list is `COPYING.NEWLIB` in the newlib sources, and in
the Arm and RISC-V toolchains the Pico SDK installs.

## In this repository only

| Component | Where | Licence |
|---|---|---|
| GNU ld's built-in RISC-V linker script, for reading | `docs/reference/ld-builtin-elf32lriscv.ld` | FSF all-permissive, notice kept in the file |

## Built separately

The ESP32-C6 firmware is Espressif's ESP-Hosted, `esp-hosted-mcu`, under the
Apache License 2.0. `esp/fruitjam-c6/build.sh` fetches and builds it; none of its
source is in this tree, and myrtos's side of the protocol (`modules/ehspi`,
`modules/ehrpc`) is written here from the protocol description. Whoever passes
on a built `ehcp.bin` should include Espressif's `LICENSE` from that checkout.

## Licence texts

### BSD-3-Clause: the Pico SDK and pico_sd_card

Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### BSD-3-Clause: lwIP

Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.

### MIT: TinyUSB, Pico-PIO-USB and wasm3

Copyright (c) 2018, hathach (tinyusb.org)
Copyright (c) 2021 sekigon-gonnoc
Copyright (c) 2019 Steven Massey, Volodymyr Shymanskyy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

### SIL Open Font License 1.1: Terminus Font

Copyright (C) 2020 Dimitar Toshkov Zhekov, with Reserved Font Name "Terminus
Font". The full text is `third_party/terminus/OFL.txt`. The fonts in the kernel
are converted from the BDF files beside it by `tools/make_font.py` and are not
called Terminus in anything myrtos displays.
