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

## What is not here yet

ESP-IDF v4.4.3 is installed on the development machine, which predates the
ESP32-C6. The slave firmware needs v5.x, so building it is a download before it
is a build.
