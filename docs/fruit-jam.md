# Setting up a Fruit Jam

From a new Adafruit Fruit Jam to a board on your WiFi, serving its sensors to a
browser. The README's "Getting started" is the short version; this is all of
it.

UbiqOS was called myrtos until September 2026. It is the same system; a board
that prints "myrtos" anywhere is running an older version.

## What you need

- **The UbiqOS file**: `ubiqos-fruit-jam-arm-chargen.uf2` from the
  [latest release](https://github.com/ulso/ubiqos/releases/latest). A release
  holds several files; this is the Fruit Jam's on its Cortex-M33 cores, and the
  one to take. `...-riscv-chargen.uf2` runs the same system on the Hazard3
  cores instead, and the `ws43b` files are for another board. `SHA256SUMS`
  beside them has the checksums.
- **A microSD card formatted FAT32.** UbiqOS reads nothing else. macOS formats
  small cards as FAT16 even when asked for FAT32, in Disk Utility and with
  `diskutil` alike; format it on Windows, or in Terminal with
  `sudo newfs_msdos -F 32 -v UBIQOS /dev/rdiskNs1`, which forces FAT32. Find
  `diskN` with `diskutil list` first and unmount the card with
  `diskutil unmountDisk diskN` -- the wrong disk is erased without a question.
- **A way to type commands**: an HDMI screen and a USB keyboard on the Fruit Jam,
  or the serial console over the USB-C cable -- `screen /dev/cu.usbmodem… 115200`
  on a Mac, PuTTY on the board's COM port on Windows.
- **For WiFi**: `ehcp.bin`, the ESP-Hosted firmware for the board's ESP32-C6,
  from the same release, with `ehcp-NOTICES.md` beside it. See step 2.

## 1. Put UbiqOS on the board

Hold **BOOT** (button 1), press **RESET**, and let go of BOOT. A drive called
`RP2350` appears on the computer. Copy `ubiqos-fruit-jam-arm-chargen.uf2` onto
it, and **press RESET when the copy is done** -- the board may stay in the
bootloader rather than restart by itself.

Copy the file; do not use `picotool load`. Picotool writes only the first of
the file's two block families, reports success, and leaves the old system in
flash.

### The network over the USB cable

This works before WiFi does. On the cable the board is **192.168.7.1** and it
gives the computer **192.168.7.2** by DHCP, so nothing needs setting up. The
board answers as `ubiqos.local` -- or the name in `config.txt`, below -- or
directly at 192.168.7.1:

    ping ubiqos.local

The cable is not a way onto the internet; the computer keeps its own
connection for that. If 192.168.7.x clashes with a network the computer already
has, give the board another address with `usb_address = 10.0.5.1` in
`config.txt`, and the computer gets 10.0.5.2.

## 2. Put ESP-Hosted on the WiFi chip

The Fruit Jam's ESP32-C6 comes with Adafruit's firmware, NINA. UbiqOS uses
Espressif's ESP-Hosted instead: the chip becomes a radio and nothing more, and
UbiqOS runs the network itself. Without it everything works except WiFi, and
the network is the cable alone.

**The file** is `ehcp.bin` in the
[latest release](https://github.com/ulso/ubiqos/releases/latest): ESP-Hosted
3.0.7 built for this board's wiring, one merged image. It is Espressif's code,
not UbiqOS's, and `ehcp-NOTICES.md` beside it -- also in the repository as
[`esp/fruitjam-c6/NOTICES.md`](../esp/fruitjam-c6/NOTICES.md) -- says what is in
it and under which licences.

**To build it yourself** instead,
[`esp/fruitjam-c6/build.sh`](../esp/fruitjam-c6/build.sh) builds the same thing;
it needs ESP-IDF 5.5 or later. It leaves four images, which go together into
the one file the board reads:

    esptool --chip esp32c6 merge_bin -o ehcp.bin \
        --flash_mode dio --flash_freq 80m --flash_size 4MB \
        0x0 bootloader/bootloader.bin \
        0x8000 partition_table/partition-table.bin \
        0xd000 ota_data_initial.bin \
        0x10000 eh_cp_wifi_sta.bin

**Writing it to the chip:**

1. Copy `ehcp.bin` to the root of the card and put the card in the Fruit Jam.
2. Start the board and check the file is there, at its full size:

       ls /sd

3. Write it:

       espflash write /sd/ehcp.bin

   It takes a few minutes -- the transfer runs at 115200 baud. **Do not cut
   the power while it runs.** The audio codec is reset as well; it shares the
   reset line with the WiFi chip.
4. Power the board off and on again, so the chip starts its new firmware.

The details, and how this came to work, are in
[docs/esp-hosted](esp-hosted/README.md).

## 3. Tell the board which network to join

Create two files in the root of the card. `config.txt`, for what the machine
is:

    hostname = fruit-jam

and `wificfg.txt`, for the network it joins:

    ssid     = your-network
    password = your-password

- The password is in clear text on the card. UbiqOS never hands either file to
  a program or to the web server, but whoever holds the card can read it.
- Both lines may also go in `config.txt`, which is where they used to live and
  where they are still read from. A card that has both is believed on
  `wificfg.txt`.
- A `#` directly after a space starts a comment. In the middle of a word it is
  an ordinary character.
- Without `hostname` the board is called `ubiqos` on the network.
- With no card, or with no password anywhere, join by hand with
  `wifi connect your-network`. The password is asked for and not shown. It is
  not kept either, so it is asked for again after every start -- unless it is
  in the key store, which is [docs/keys.md](keys.md).

All the keys, and why they behave as they do, are in [docs/config.md](config.md).

Restart the board. The screen should say something like:

    wifi: joined
    wifi: address 192.168.x.y mask … router …
    ntp: the time is …

## 4. Try it

From a computer on the same network:

    ping fruit-jam.local

On the board, a web server -- the sensors at `/`, the card at `/files`, the
board's own figures at `/api/status`:

    httpd &

The sensors need a BleuIO dongle in one of the USB-A ports and the scanner
running in the background:

    hibouair -q &

Then open `http://fruit-jam.local/` in a browser. And the board can reach the
internet's servers over TLS, checking their certificates:

    fetch https://example.com/

## 5. Have them start by themselves

The file `startup` in the root of the card is run line by line at every start.
With these two lines in it the sensors are served from power-on, with nobody
typing anything:

    hibouair -q &
    httpd &

Write it on a computer with the card in a reader -- the name is `startup`, with
no `.txt` -- or on the Fruit Jam itself:

    echo hibouair -q & > /sd/startup
    echo httpd & >> /sd/startup
    cat /sd/startup

(An `&` means "in the background" only at the end of a line, so here it goes
into the file instead.) Both wait up to 30 seconds for the dongle and the
network, which are not ready yet when the file runs.

To stop them, from the keyboard or the serial console:

    kill hibouair
    kill httpd

## Back to Adafruit's firmware

To give the WiFi chip NINA back: put Adafruit's `SerialESPPassthrough.ino.uf2`
on the board through BOOT and RESET as in step 1, write the NINA image with
esptool following Adafruit's instructions for the Fruit Jam, and then put
UbiqOS back.
