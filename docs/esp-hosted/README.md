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
