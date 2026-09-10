# ESP-Hosted on the Fruit Jam: the wiring

The ESP32-C6 on this board ships with NINA firmware, which carries its own
TCP/IP stack. `modules/wifilib` talks to it in sockets: connect to this network,
open this socket, send these bytes. That is why WiFi and lwIP are two different
worlds here, and why a socket number has a stack in its high byte.

ESP-Hosted is the other arrangement. The chip becomes a radio and nothing more:
it hands over raw 802.11 frames as Ethernet frames, and the host runs the stack.
For this project that is the interesting one, because the host already runs a
stack -- the same lwIP that answers ping over USB today -- and a second netif
costs almost nothing next to a second protocol.

Getting there means new firmware in the C6, which means knowing exactly how the
two chips are wired. Adafruit's own pinout page has one detail wrong (it says
the C6's bootloader strap is on BUTTON1), so what follows is read out of the
schematic itself -- `Adafruit Fruit Jam.sch` in adafruit/Adafruit-Fruit-Jam-PCB,
which is Eagle XML and can simply be parsed.

## The pins, both ends

| net           | RP2350 | ESP32-C6   | what it is                       |
|---------------|--------|------------|----------------------------------|
| `SCK`         | GP30   | IO22       | SPI clock                        |
| `MOSI`        | GP31   | IO21       | host to chip                     |
| `ESP_MISO`    | GP28   | IO6        | chip to host                     |
| `ESP_CS`      | GP46   | IO7        | chip select (also `IC1 /OE`)     |
| `ESP_BUSY`    | GP3    | IO18       | handshake, NINA's ACK            |
| `I2S_ESP_IRQ` | GP23   | IO9/BOOT9  | second handshake AND the strap   |
| `PERIPH_RST`  | GP22   | EN         | reset, SHARED with the audio DAC |
| `D8/TX1`      | GP8    | RXD0       | host to chip, UART               |
| `D9/RX1`      | GP9    | TXD0       | chip to host, UART, through R28  |

Three of those rows are the whole story.

**GP23 is the download strap.** It is the C6's IO9, which is what this chip
samples at reset to choose between running the application and waiting for the
serial bootloader -- low means wait. It is pulled up by R27, so nothing has to
happen for a normal boot. It is also wired to the audio DAC's GPIO1, which is
why the SDK's board header calls it `I2S_ESP_IRQ`: two owners, and whoever
drives it has to know about the other.

**GP22 resets the DAC as well.** Reflashing the C6 takes the audio chip down
with it, and the audio driver has to be told rather than left to find out.

**The UART is on the header pins.** GP8 and GP9 are D8 and D9 on JP3, so a
passthrough is also visible from outside, and a probe on those pins sees what
esptool sees.

## What reflashing looks like from here

1. GP23 low -- the strap.
2. GP22 low, wait, GP22 high -- the reset that samples it.
3. GP23 released once the chip is up. It is an input to the ROM only at reset,
   and leaving it low sits on the DAC's line for no reason.
4. UART1 at 115200 on GP8/GP9, bridged raw to the host's CDC.
5. On the host, esptool with `--before no_reset --after no_reset -c esp32c6`,
   because steps 1 to 3 have already done what its own reset sequence would.

Adafruit's own recovery route is the same shape and does not involve myrtos at
all: their `SerialESPPassthrough` UF2 over BOOTSEL, esptool, then flash myrtos
back. That is the way back if the passthrough here is what breaks.

## What the transport can be

ESP-Hosted's full-duplex SPI wants CLK, MOSI, MISO, CS and TWO more lines:
HANDSHAKE and DATA_READY. This board has exactly two spare -- GP3 and GP23 --
which is unlikely to be a coincidence. The cost is that DATA_READY would then
share GP23 with the DAC's GPIO1 and with the strap.

Its UART transport wants nothing but GP8 and GP9, which are already there for
flashing. Slower, and the obvious first step: it proves the framing and the
netif with none of the pin contention, and SPI becomes a transport swap rather
than a bring-up.

## The co-processor firmware

`esp/fruitjam-c6/` builds it: Espressif's own `examples/wifi/sta/cp` with this
board's wiring in `sdkconfig.defaults.fruitjam`. Verified to reach the image
rather than assumed --

    CONFIG_EH_TRANSPORT_CP_SPI=y
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_MOSI=21
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_MISO=6
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_CLK=22
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_CS=7
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_HANDSHAKE=18
    CONFIG_EH_TRANSPORT_CP_SPI_GPIO_DATA_READY=9

-- and SPI had to be asked for: the C6 has an SDIO slave, so ESP-Hosted picks
SDIO by default, and this board has no SDIO wiring to that chip at all.

It builds to 1 131 936 bytes at 0x10000, with a bootloader, a partition table
and an OTA data block below it, and needs ESP-IDF v5.5 or later.

## The control path is not free

ESP-Hosted carries two things over the bus. The DATA path is Ethernet frames,
which is what this project wanted and is nearly free -- lwIP already has a
netif over NCM and a second one costs almost nothing. The CONTROL path is not:
scan, connect, "what is my address" are RPC, and Espressif's porting guide
expects a host to take their whole stack -- `esp_event`, `esp_netif`, their
lwIP glue and an OS port layer. That does not fit in sixty kilobytes.

So the control path has to be written here, against the wire format rather than
against their API. There is an `eh_tlv` serializer beside the protobuf one which
looks like the cheaper way in; that is the next thing to read.

## The route is proved

`espflash sync`, 10 September 2026, on the board:

    --- what the ROM says ---------------------------------------
    ESP-ROM:esp32c6-20220919
    Build
    -------------------------------------------------------------

    SYNC..
    synchronised: 12 bytes back, 01 08 04 00 07 07 12 20 00 00 00 00

    The ROM loader is listening. The wire, the strap and the reset all work.

The frame reads back as the protocol says it should: `01` a response, `08`
SYNC, size `0004`, value `20120707`, and four zero status bytes. Afterwards
`wifi` answered "ESP32-C6 firmware 3.3.0" again -- the strap is only read as
the chip leaves reset, so a reset puts NINA back and nothing was written.

The first run of this printed 32 bytes of `W (318) spi_flash: Detected size`
under "what the ROM says". That is NINA's own log in ESP-IDF's format, which a
ROM never prints, and it was exactly 32 bytes because that is the depth of the
RX FIFO -- stale, from before the reset. `espflash` drains the FIFO first now.
A proof that prints leftovers as evidence is worse than no proof.

## The C6 runs ESP-Hosted

Written 10 September 2026 with `espflash write /sd/ehcp.bin` -- one merged
image, 1 197 472 bytes, at offset 0. The chip came up talking:

    ehcp_core: auto_feat_init ... 'feat_wifi' (priority 200)
    ehcp_rpc_reg: ++ RPC registered: req [0x0101,0x018c]
    ehcp_core: ESP-Hosted coprocessor ...
    wifi_mcu_example Starting...

and `wifi` now answers "handshake never moved, GP3 ack=1" -- NINA is gone, and
the WiFiNINA driver is talking to a chip that no longer speaks its protocol.
That is the expected state, not a fault. Adafruit's SerialESPPassthrough UF2
plus their NINA .bin is what puts it back.

The image is built by `esp/fruitjam-c6/build.sh` and merged with

    esptool --chip esp32c6 merge_bin -o ehcp.bin \
        --flash_mode dio --flash_freq 80m --flash_size 4MB \
        0x0 bootloader/bootloader.bin \
        0x8000 partition_table/partition-table.bin \
        0xd000 ota_data_initial.bin \
        0x10000 eh_cp_wifi_sta.bin

so that the board has one file to read instead of four offsets to be told
about. It goes onto the card with `usbdisk`.

### What is worth knowing about the writing

* The flash size is written down, not asked for. `SPI_SET_PARAMS` has to be
  told 4 MB because nothing on this wire can ask the chip how big its flash is
  without the stub loader, and the module on this board is an
  ESP32-C6-MINI-1 with four.
* `FLASH_BEGIN` takes TWENTY bytes here, not sixteen. The ESP32-S2 and
  everything after it want a fifth word saying the write is not encrypted --
  but only when there is no stub loader. Sixteen gets "the packet was the
  wrong size" and nothing else to go on.
* Every block is a full kilobyte, the last one padded with 0xff. The ROM was
  told how many to expect and will not take a short one.
* 115200 baud, which is what the ROM listens at. `CHANGE_BAUDRATE` would make
  this eight times quicker and is the obvious next improvement.
* The reply's payload IS its status, and the last two bytes are the code and
  the reason. Reading them from the END is what makes the same code work on a
  chip that answers with two and one that answers with four.

### The log, whole

The first version of the driver read the UART's FIFO when somebody asked, and
that FIFO is thirty-two bytes -- two and a half milliseconds at 115200. The
boot log arrived in pieces. It now has a handler at priority 0x40 and an eight
kilobyte ring, and `espflash log` prints all 5792 bytes with nothing dropped.

Which matters for more than tidiness, because the chip reads the wiring back:

    SPI_DRIVER: transport[cp]: SPI ctrl=1 mode=3 MOSI=21 MISO=6 CLK=22 CS=7
                HANDSHAKE=18 DATA_READY=9

Every number there was read out of the schematic and is now confirmed by the
chip that has to live with it. Two more facts for the host end: **SPI mode 3**,
and `Freq:ConfigAtHost` -- the co-processor takes whatever clock the host
gives it rather than naming one.

The handler follows the same rule as modules/adc's: above the kernel's
threshold, so a trap with interrupts off cannot cost bytes, and touching
nothing but its own ring. There is no lock because there is nothing to lock --
the handler writes the head, the reader writes the tail, and each reads the
other's word. `MYRTOS_SS_ESP_STATS` reports what was dropped, so a log with
holes says so rather than looking merely short.

## The transport is up

`ehstat`, 10 September 2026:

    transactions   2
    frames in      1
    dummies in     1
    bad checksum   0
    bad header     0

    the co-processor announced itself
      chip                   0x0d          ESP32-C6
      capabilities           0xa0          WLAN over SPI, checksum on
      its receive queue      0x0a
      its send queue         0x0a
      firmware               3.0.7
      extended capabilities  0x00000010    WLAN supported
      RPC version            0x02

Which is the whole of Espressif's bring-up step 1: the transport comes up and
the host receives the co-processor's init event. Every term of the link is
what the firmware we built said it would be.

### Why it has a thread

Every exchange is a FIXED 1600 bytes whatever the payload, because the
co-processor arms its slave for that much and a shorter clocking leaves it
half fed. At 8 MHz that is 1.6 milliseconds, and a driver's read and write run
in the trap handler with interrupts off. So the transaction lives in a kernel
thread of the driver's own -- the shape modules/wifilib already uses -- and
the device's read and write only move bytes to and from rings.

### Two ways to freeze the board, in one afternoon

The thread is at priority 21, above the shell at 16, and it wedged the machine
twice. Both times the answer was already written in modules/wifilib, which I
had read that morning for the pin numbers.

The first time the sleep was at the bottom of the loop and a `continue`
skipped it, so a burst could be taken as a burst. With handshake and data
ready both high that is back-to-back 1.6 ms transactions and the shell never
runs again.

The second time the sleep was unconditional and it still wedged, because
`K->sleep_ms` is the SDK's and BUSY-WAITS. A kernel thread that spins never
reaches the scheduler; it is only preempted where it makes a system call.
`wifilib.c:98`: *"seconds of spinning is exactly what froze the machine when
sleep_ms was used instead of myrtos_sleep"*.

Both times the way back was the BOOTSEL button. Both times lwIP over USB kept
answering ping, because the USB task sits above 21 -- which is worth knowing:
a board that pings is not a board that is alive.

### The dummy comes before the header check

The first run counted `bad header 1` and `dummies 0`. A dummy frame says only
"nothing this time" -- interface type `ESP_MAX_IF`, length zero -- and nothing
promises its offset field says twelve. Checking the offset first counted every
dummy as a broken header, and the first exchange of the link was reported as a
fault. Recognising the dummy first turned it into `dummies 1, bad header 0` --
which is how it was confirmed rather than argued. The driver now also keeps the
first twelve bytes it could not read, so the next one is a fact and not a
guess.

### What is deliberately slow

One transaction per tick. That is 1600 bytes a millisecond, 1.6 megabytes a
second, and far more than this link will carry -- but it is a poll, not an
interrupt on handshake, and the interrupt is the next improvement. 8 MHz is
what the NINA driver ran these same pins at for months; the co-processor takes
whatever the host gives it (`Freq:ConfigAtHost`) so that number is ours to
raise.

## Why not UART

UART needs no extra pins and GP8/GP9 are already there. Espressif's own design
note is why not:

> The framing has no delimiter, escaping, or magic-byte resync -- a single
> dropped byte desynchronizes the stream with no marker to recover from.

This project has already paid for that once, when a BLE scan and the web server
killed the WiFi link and the cause turned out to be a protocol with no way back
in sync. SPI puts a CS edge around every transaction, and the board has exactly
the two spare lines full-duplex SPI asks for.

## The way back

Adafruit's `SerialESPPassthrough.ino.uf2`, from the AirLift page linked above.
It goes on over BOOTSEL and does not involve myrtos at all, so it is still the
way back when it is myrtos's own passthrough that is broken. It is deliberately
NOT committed here -- it is 185 kB of somebody else's binary and .gitignore
says so -- and lives in tools/ on the development machine.
