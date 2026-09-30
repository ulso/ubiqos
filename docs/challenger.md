# The Challenger+ RP2350 WiFi6/BLE5

iLabs' [Challenger+ RP2350 WiFi6/BLE5](https://ilabs.se/product/challenger-rp2350-wifi-ble/)
is a Feather-sized board: an RP2350A with 8 MB of flash and 8 MB of PSRAM, an
ESP32-C6 beside it for WiFi 6 and Bluetooth LE, a LiPo charger, and a USB-C
socket. UbiqOS runs on it with no screen and no card, and out of the box it is
a **BLE-to-WiFi bridge**: it listens for HibouAir air-quality sensors on
Bluetooth and serves what they say on a web page, over WiFi.

    http://challenger.local/              the sensors, as cards that update
    http://challenger.local/api/sensors   the same, as JSON
    http://challenger.local/api/status    memory, processes

## What UbiqOS uses

| | |
|---|---|
| USB-C | the console (`/dev/cu.usbmodem…` on a Mac) and a network over the cable: the board is 192.168.8.1 and hands the computer an address by DHCP |
| ESP32-C6 | WiFi and Bluetooth LE, over SPI1 and ESP-Hosted. UbiqOS runs the network itself; the C6 is a radio |
| PSRAM | 8 MB, CS on GPIO0 |
| Flash | 8 MB: the kernel, the programs from 1 MB, the key store at the top |
| UART0 | GP12/GP13 on the header, a diagnostic serial line |

There is no SD card. What a Fruit Jam keeps on its card lives in flash here:
the WiFi password in the key store, which this board opens by itself at every
start ([keys.md](keys.md)), and the boot script, as a module --
[`boards/challenger-startup.c`](../boards/challenger-startup.c):

    hibouair -q &
    httpd &

`hibouair` finds no BleuIO dongle on this board and listens through the C6's own
Bluetooth controller instead, as raw HCI; `httpd` serves what it hears. The
board header is [`boards/challenger-rp2350.h`](../boards/challenger-rp2350.h).

## 1. Build and load UbiqOS

As in the [README](../README.md), with the board chosen:

```bash
cmake -S . -B build-challenger -G Ninja -DUBIQOS_BOARD=challenger
ninja -C build-challenger
```

Hold the **BOOTSEL** button, press **RESET**, and let go: the board appears as a drive called
`RP2350`. Copy `build-challenger/ubiqos.uf2` to it. Once UbiqOS runs, `bootsel`
at its console does the same without the buttons.

## 2. Put ESP-Hosted on the C6

The C6 ships with Espressif's AT firmware, which has an IP stack of its own.
UbiqOS needs ESP-Hosted, with the Bluetooth controller, built for this board's
wiring:

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
esp/challenger-c6/build.sh ~/esp/ehcp-challenger ble
```

It prints the four images and their offsets; merge them into one file with
`esptool merge_bin`, as in [fruit-jam.md](fruit-jam.md#2-put-esp-hosted-on-the-wifi-chip).

With no card, the file goes to the board over the cable. On the computer, in
the directory that holds it:

```bash
python3 -m http.server 8000
```

and at the board's console (the computer is 192.168.8.2 on the cable):

    fetch -o /tmp/ehcp.bin http://192.168.8.2:8000/ehcp.bin
    espflash write /tmp/ehcp.bin

It takes a few minutes at 115200 baud. **Do not cut the power while it runs.**
Then press RESET. `ehrpc bt` should answer `bluetooth init: ok` and
`bluetooth enable: ok`.

iLabs publish the AT firmware, and ESP-Hosted builds of their own, on the
board's product page -- that is the way back.

## 3. Join WiFi

At the console, with the network's name in place of `your-network`:

    key set wifi.your-network

The password is asked for and not shown. It is sealed in the key store, and the
board joins by itself at every start from then on -- 7 to 8 seconds after a
reset on the bench:

    wifi: joined
    wifi: address 192.168.x.y mask … router …

`key set ssh.password` does the same for `sshd`, which then starts at boot too:
`ssh challenger.local`.

## 4. The bridge

Nothing to do: after a reset the boot script starts it. `hibouair` waits for
the WiFi join to settle first -- the join and the Bluetooth set-up share one
channel to the C6 -- and then scans. On the bench it hears five sensors within
seconds of starting, and WiFi keeps working while it listens: pinging the board
lost nothing over a 20-second scan.

`kill hibouair` and then `hibouair` without `-q` draws the same table at the
console -- one scanner at a time, since both would read the same controller.
`blescan 20` lists every Bluetooth LE device in earshot for twenty seconds.

Running on the battery is the next thing to try.

## Notes on the hardware

- **The debug connector J4 has SWCLK and SWDIO swapped.** The schematic (rev
  P1.2) gives pin 1 SWCLK, 2 GND, 3 SWDIO -- the Raspberry Pi debug pinout --
  and a debug probe wired that way gets no answer at all. Crossed, a J-Link
  connects at once. J-Link reads and debugs this chip but cannot write its
  flash; images go on over USB.
- **The RP2350 on these boards is an A2** (CHIP_ID `0x20004927`), with
  erratum RP2350-E9: an input with its pull-down enabled can latch high.
- **The C6's EN (GP15) and boot strap (GP14) have no pull-ups on the board.**
  Left to themselves they read low after the RP2350 lets go of them, and the
  C6 stayed in reset; UbiqOS turns the RP2350's own pull-ups on for both.
- **The schematic asks for ESP32_MISO (GP8) to be high while the C6 leaves
  reset**, and it is pulled up for that too.
