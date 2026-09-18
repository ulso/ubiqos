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

Adafruit's own recovery route is the same shape and does not involve UbiqOS at
all: their `SerialESPPassthrough` UF2 over BOOTSEL, esptool, then flash UbiqOS
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
other's word. `UBIQOS_SS_ESP_STATS` reports what was dropped, so a log with
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
sleep_ms was used instead of ubiqos_sleep"*.

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

### An interrupt on handshake would buy nothing

That was written here as "the next improvement", and it was wrong about this
kernel. There is no way to wake a thread from an interrupt in UbiqOS, and that
is a design decision rather than a gap. `ubiqos_wake_readers`
(kernel/scheduler.c:932) runs from the TIMER TICK and polls every blocked
process's device:

> Called from the timer tick. A device driver knows whether it has anything
> waiting; asking it once per millisecond costs the kernel a few comparisons
> and costs the blocked process nothing at all.

modules/gpio says the same from the other side (gpio.c:31): its handler "may
not wake anybody -- see the rule beside irq_install -- and it does not need
to: the scheduler already polls readable". The tick is the wake mechanism for
everything in this system, so an edge interrupt on handshake could set a flag
a millisecond earlier and the thread would still not run until the tick. And
handshake is held high until the host services it, so no edge is ever missed
by sampling the level.

### What actually costs something

A 1600-byte exchange, measured on the board:

| clock | typical | worst |
| ----- | ------- | ----- |
| 8 MHz | 1720 us | 2070 us |
| 32 MHz | 534 us | 912 us |

with the announcement decoding and no checksum errors at either. The clock is
now 32 MHz. Espressif allow up to 40 for this chip and take their own
reference numbers there.

534 us of that was CPU **held at priority 21, above the shell**, because the
SDK's `spi_write_read` polls a byte at a time. It is on DMA now:

    one exchange, microseconds
      on the wire   999
      on the CPU      1   (worst 13)

Two numbers rather than one, because they stopped being the same thing. The
wire figure includes the tick the thread sleeps through while the transfer
runs; it is latency and a millisecond of it costs nothing. The CPU figure is
what the processor spends above the shell, and that is what went from 534
microseconds to one.

The transfer size is what makes this possible: an exchange is ALWAYS 1600
bytes, whatever the payload, because the length lives in the header and not in
the stream. Nothing is escaped and nothing is framed, so how many bytes are
coming is known before the first one moves. The SLIP the ROM loader speaks
over UART is the opposite -- a byte can become two and the end is only known
when it arrives -- and no DMA could be pointed at it.

Two details that are not optional:

* The buffers come from `K->driver_alloc`, not from the driver's own statics.
  A module's memory is in the module pool, which is PSRAM, and DMA to PSRAM is
  not reliably visible to the CPU afterwards. `K->dma_safe` is then ASKED
  rather than assumed, because the code doing the DMA is not the code that
  knows the memory map.
* Both channels start in one `dma_start_channel_mask` write, so the receive
  side is armed before a byte can arrive. Started one after the other, the
  first bytes clocked in have nowhere to go and every frame afterwards is one
  short.

The wire figure is a whole tick because the thread starts the transfer and
sleeps: the exchange takes two ticks and the processor is in none of it. One
frame per two milliseconds is 800 kB/s, which this link will not reach for
other reasons long before it matters.

## The control plane answers

    ubiqos:/> ehrpc mode
    mode 0  (off),  the chip answered 12289 -- the radio is not initialised

12289 is 0x3001, the first of the WiFi driver's own error codes, and it is the
right answer: nothing has called WifiInit yet. Which means the whole round trip
worked -- request framed, carried, unwrapped, dispatched, `esp_wifi_get_mode`
actually run on the co-processor, and its real return value carried back.

### What it took, and what nearly did not

**The handshake is not optional.** The first request went out, the chip took it
and said nothing at all, and there was nothing wrong with the frame. The
co-processor announces itself and then WAITS for the host to answer on
`ESP_PRIV_IF` before it will do anything else. Espressif say so plainly --
"getting this right is the whole game; if it does not complete, feature
debugging is premature" -- and it is now done by the driver, unasked, the
moment the announcement arrives.

Two TLVs are deliberately NOT sent in that answer. `0x1A`, the RPC version, is
a strict match on the far side and a mismatch calls `abort()`: the chip
reboots. `0x23`, the RPC version ack, reads anything that is not V3 as V1 and
would talk the link down a protocol. Neither is needed -- the chip opens its
RPC endpoints on receiving any init event from the host.

**The protobuf is not what goes on the wire.** It is wrapped:

    [0x01][ep_len:2 LE]["RPCRsp"][0x02][data_len:2 LE][protobuf]

No document said so. The chip did. After the handshake it sent an event nobody
had asked for, and reading its twenty bytes -- rather than arguing about the
encoder -- showed `01 06 00 R P C E v t 02 08 00` and eight bytes of protobuf.
Espressif's own host carries the same shape in a comment in
`eh_host_mcu_vserial.c`, which was worth finding afterwards and would have been
no use before: the question was not answered anywhere the search had been
looking. The endpoint a REQUEST goes to is called `RPCRsp`, which reads
backwards and is not a mistake.

### The encoder is forty lines

ESP-Hosted's RPC is one protobuf message with a number in it:

    Rpc { msg_type=1  msg_id=2  uid=3  <msg_id> = the request itself }

The payload sits at the field number that IS its message id, so finding it
needs no table, and a response is the request's id plus 256. Unknown fields
are skipped by their wire type, which is what lets forty lines read a message
they were never compiled against -- and `Rpc` has a hundred and forty possible
payloads, of which this knows one.

That is why the control plane did not need Espressif's host stack. Their
porting guide expects `esp_event`, `esp_netif`, their lwIP glue and an OS port
layer, and none of it would fit in sixty kilobytes.

### Blocking reads froze the board

`/dev/eh` answers `readable`, and a read of a device with nothing in it WAITS.
`ehrpc` read blind in a loop, and a process parked for ever on an answer that
was not coming took the console with it. It asks `ubiqos_readable` first now,
as `espflash` does of `/dev/esp` -- and that was found by bisecting rather than
by guessing, which is why the guess about the send path never had to be made.

## The radio comes up

    ubiqos:/> ehrpc up
    starting the radio ... ok
    station mode       ... ok
    start              ... ok
    ubiqos:/> ehrpc mode
    mode 1  (station),  the chip answered 0 -- ok

`WifiInit`, `SetMode` and `WifiStart`, each an `esp_wifi_*` call running on the
co-processor, and the mode read back afterwards to prove it stuck. `ehrpc
connect <ssid>` adds `WifiSetConfig` and `WifiConnect` on top; the password is
typed there, never echoed, never an argument, and wiped before the process
ends -- the same rule `wifi connect` has always had, unchanged by the change of
radio.

Four things went wrong on the way, and each one looked like something else.

**`esp_wifi_init` takes longer than two seconds.** It reported "no answer", and
the next command then read the LATE reply and reported the mode as "type 2, id
534" -- which is 278 + 256, WifiInit's own response, arriving a command too
late. Each step now says how long it may honestly take, and every call drains
whatever is waiting before it asks, because a stale answer is worse than none.

**Events interleave with responses.** The chip pushes them on the same
interface whenever they happen, which is in the middle of a request as often as
not. Reading one frame and judging it was enough while the link was silent and
stopped being enough the moment the radio was doing something. A call now reads
until the answer to ITS question arrives.

**The inbox was one frame deep**, on the reasoning that a request gets one
answer. `WifiStart` appeared to go unanswered for ten seconds while its reply
had been sitting there and been overwritten by the very event it caused. It is
four deep now, and what it drops is counted.

**A getter's result is not at field 1.** Every action -- `WifiInit`, `SetMode`,
`WifiStart`, `WifiConnect` -- answers `int32 resp = 1`, so that is where the
generic call looks. A getter puts the value it was asked for there and its
result at field 2, and printing the generic answer said "the chip answered 1"
when 1 was the mode.

### What the config file cannot do yet

`/sd/config.txt` holds an SSID and a password, and the kernel will not hand
those bytes to a process -- which is the whole point of how that file is
treated. The NINA path got round it by having the KERNEL do the joining, and
the same has to happen here: the RPC built where the password already is, in
the driver, rather than in `ehrpc`. Until then `ehrpc connect` asks.

## The data plane

Station frames are ordinary Ethernet and go to lwIP -- the same lwIP that has
been answering ping over USB since the 9th. `kernel/ehnet.c` is the second
netif, and the arrangement is decided by one constraint:

**lwIP may be touched from the USB task and nowhere else.** `NO_SYS` is 1, so
the stack has no locking of its own, and the transport is a kernel thread at
priority 21. Two contexts must never both be inside it. So the driver QUEUES
what arrives, eight frames deep, and the USB task drains that queue on its own
turn. The queue is the boundary between the two, and it is the only one.

The frames do not travel through `read` and `write`: those already carry the
control plane, and a driver module serves exactly one device. So the data plane
is `getstat`/`setstat` -- `UBIQOS_SS_EH_RX` takes the next frame and answers
its length, `UBIQOS_SS_EH_TX` queues one to send.

The draining is bounded at eight frames a turn. A burst of broadcast traffic is
not a reason to stop answering USB for as long as it lasts, and what is left
waits a millisecond.

### The address, and the one that took two tries

DHCP on the WiFi interface, AutoIP on the USB one. There is a real router on
one and one host with no server on the other, and each gets the answer that
suits it. Both are compiled in now; the measurement is above.

The netif needs the station's OWN hardware address -- the co-processor turns
802.11 into 802.3 using the address the access point knows, and a netif with
any other discards everything meant for the machine it is part of. That takes
RPC 257, so `ehrpc up` fetches it and hands it to the driver with a setstat:
the protobuf stays in one place and the driver keeps six bytes.

`Rpc_Req_GetMacAddress` has a field called `mode`, and the co-processor hands
it straight to `esp_wifi_get_mac`, which takes an INTERFACE. Station is 0
there; 1 is the access point. Sending `WIFI_MODE_STA` asked for the address of
an interface that had never been started, and the answer to that was silence
rather than an error -- which reads exactly like a link that has stopped
working.

    ubiqos:/> ehrpc up
    starting the radio ... ok
    station mode       ... ok
    start              ... ok
    its address        ... 58:e6:c5:f5:7a:ac
    ...
    wifi: interface up, asking DHCP for an address

## It answers ping over the air

    64 bytes from 192.168.68.54: icmp_seq=0 ttl=255 time=68.090 ms
    6 packets transmitted, 6 packets received, 0.0% packet loss

DHCP took an address from the house router and the board answers on it, over
esp-hosted, into UbiqOS's own lwIP. The rebuild is usable.

### Power save was most of the latency, and not all of it

The first measurement was 189 to 272 ms, averaging 232, and it got BETTER when
pinged ten times a second. Hundreds of milliseconds, improving under load, is
beacon intervals -- and ESP-IDF's own header says why: "Default power save type
is WIFI_PS_MIN_MODEM". `esp_wifi_init` leaves the station asleep between DTIM
beacons and a packet for us waits for the next one. It looks like a slow bus
and it is a sleeping radio.

`SetPs(WIFI_PS_NONE)` in `ehrpc up`, and the idle case went **232 ms to 74**,
min 68, with the variance gone. Confirmed rather than argued.

But 68 ms is still a floor, and under load it is worse -- three runs at ten
pings a second gave averages of 233, 151 and 158 against 74 at idle. A
transport whose exchange is two ticks should not produce either number. So the
driver now counts what an outbound frame waits for: microseconds from being
handed over to going out, and how many turns found the handshake low while
something was queued. That is the direct question -- is the wire slow, or is
nobody offering us a turn -- and it is not going to be guessed at a third time.

### A host reset takes the radio with it

`ehrpc mac` was written to save retyping a password after a reboot: nothing in
UbiqOS touches the chip's EN pin, so its association ought to outlive a host
reset. **It does not.** The co-processor watches the host, tears the radio down
when it restarts, and re-announces itself; after a UbiqOS reboot it answers
"the radio is not initialised".

Which makes the `/sd/config.txt` path matter far more than it looked. It is not
a convenience: it is the difference between a board that joins its network on
boot and one that needs somebody at the keyboard every single time it restarts.
And it cannot be done from a process, because the kernel will not hand a
password to one -- so the join has to move into the driver, where the password
already is.

## The join is the kernel's, so the card can hold the password

The whole sequence -- initialise, station mode, power save off, the network,
start, connect, and its address afterwards -- lives in the driver now, in a
thread of its own. It moved for one reason: `/sd/config.txt` holds a password
and the kernel will not hand those bytes to a process, so the message that
carries it has to be built where the password already is. That is the same
arrangement `modules/wifilib` has had since the NINA days, and it is why the
board can now come up on its own network instead of waiting for somebody at
the keyboard -- which matters, because the co-processor tears its radio down
every time the host restarts.

`ehrpc connect` kept the typing and gave up the sequence: there is one
implementation of a join and both routes go through it.

### "joined" was a lie, and a fake network found it

The first version reported joined the moment `WifiConnect` returned zero, and
said it just as cheerfully for a network called `nosuchnetwork` with a made-up
password. `esp_wifi_connect` returns as soon as it has started TRYING; whether
it worked arrives later as an event.

So the driver asks now -- `WifiStaGetApInfo`, which is the direct question and
answers with an error until there is an access point to name:

    ubiqos:/> ehrpc connect nosuchnetwork
    password:
    joining...........
    it did not join -- the console log says why

A status line that says joined when it is not is worse than no status line.

### The remaining latency is not the transport's

The driver now counts what an outbound frame waits between being handed over
and going out:

    last 975 us, worst 2917
    turns ready 70, turns blocked 192

So the handshake is low nearly three turns in four when we have something to
send -- and it does not matter, because the co-processor offers a turn within
a millisecond anyway. Inbound costs a tick more, since the USB task drains the
queue on its own turn. Call it three milliseconds of UbiqOS, measured.

Against the same router, from the same Mac:

    the router  8.4 ms
    the board  79.0 ms

Seventy milliseconds are spent somewhere between the two, and none of them are
in the transport, the queue or the netif. What is left is the co-processor and
the air, and neither can be decomposed further from this end without a capture.
Power save is off; that was the 232 ms.

## A page over the air

    over the air   200, 4003 bytes in 0.74 s
    over USB       200, 4003 bytes in 0.05 s
    by name        200, 4003 bytes

Four faults stood between the netif working and this, and three of them looked
like something else.

### An MTU is not a frame

`link_output` sees the FRAME -- the ethernet header on top of what IP was
allowed to put in it -- so a 1500-byte buffer is fourteen bytes short of every
full-length segment. The length was checked against the same wrong number, so
nothing overflowed: it was a silent refusal of exactly the packets that matter.
Small ones went out and large ones did not, which is why httpd's headers
arrived, its 4003-byte body never did, and the connection sat open. Ping worked
throughout, and that is what made the link look whole.

### One frame out is not a queue

The outbound side held one frame and refused the rest, on the control plane's
reasoning: one request, one answer, and lwIP will retry. TCP does not work that
way -- it sends a window at a time, and a refusal is not a pause but a lost
segment waiting on a retransmission timeout measured in seconds. Four deep now.

### Deciding twice is a race

The thread chose what to send when a transfer STARTED and ticked it off when it
FINISHED, a tick later, by asking the same questions again. A control frame
queued in between was marked as sent without ever leaving -- "connect got no
answer" for a request that had been struck off the list. It remembers what it
sent now.

### A status that has not been reset is the previous answer

`join_state` was set to "trying" by the thread, which wakes on a fifty
millisecond poll. A second attempt after a failure left the old answer standing
for those milliseconds, and the caller read it and reported the new attempt as
failed before it had begun. Ulf's word for it was *"direkt"*, and that one word
is what said it was the status line and not the join.

### And lwIP's heap was too small

`MEM_SIZE` was 4000, tuned for one interface with no DHCP client. There are two
now, each with an mDNS registration, and a DHCP client on one. A stack that
cannot allocate a segment does not report anything: it stops, and the client
waits. 8000.

## What the link costs

    transactions 595   frames in 343   out 38
    lost 0   refused 0
    a frame waiting to go out: last 1007 us, worst 99095
    turns ready 38, turns blocked 178

Nothing is dropped and nothing is refused, so the queues are the right size.
The median outbound frame waits a millisecond. The WORST waits ninety-nine,
and the reason is in the last line: the handshake was low on 178 turns out of
216, because the host may only clock a transaction when the co-processor
offers one. That is the tail, and it is what makes a page take 0.74 seconds
over the air and 0.05 over the cable.

## The router's device list, settled

The board did not appear in the TP-Link Deco app although it was pingable and
had an address, and there were three possible reasons: the lease was not real,
the name was not being sent, or the app was not showing it.

The first two are answered. The lease brings a netmask of 255.255.252.0 and a
router of 192.168.68.1, and a stack cannot invent those -- they come from the
server. And a capture on the host settles the second:

    DHCP-Message (53): Discover
    Hostname (12), length 6: "ubiqos"
    ...
    DHCP-Message (53): Request
    Requested-IP (50): 192.168.68.54
    Server-ID (54):    192.168.68.1
    Hostname (12), length 6: "ubiqos"

The name goes out in both, from the right MAC, and the address asked for is the
address given. That is a correct exchange from a client that says what it is
called, so what remains is the app's own choice about what to list, and there
is nothing on this side left to fix.

Worth having proved rather than assumed: two of the three explanations were
about UbiqOS, and both were wrong.

## The hundred milliseconds are not ours

A ping from the board to its own router sits on a FLOOR of 102.9 ms with very
little jitter -- ten of them came back 102.9, 102.9, 137.9, 102.9, 106.9 --
and a floor with low jitter is a mechanism rather than congestion. The same
floor appears in both directions and to every peer: the Mac reaches the router
in 15 ms and the board in 117.

It is not the transport, and this is measured rather than argued:

    handshake high 34633    handshake low 266      (99.2 per cent armed)
    turns blocked      0    us since last 933 us
    a frame waiting to go out: last 851 us

The co-processor is armed and offering a turn essentially every millisecond,
and an outbound frame leaves in under one. Inbound costs one more tick. Call it
three milliseconds of UbiqOS in a 103 ms round trip.

An earlier reading of these counters said "blocked on 178 turns out of 216",
and that was a ratio of two numbers that are not comparable: both only count
turns where there was something to send. Counting the handshake
unconditionally is what settled it, and it said the opposite.

Two explanations were tested and are dead:

* **Power save.** It was real and it was worth 158 ms -- an idle ping was 232
  before `WIFI_PS_NONE` and 74 after. But the chip now answers `power save 0`
  when asked, and setting it a second time AFTER the association changed the
  floor by nothing at all. The line that did it was taken out again.
* **The co-processor polling on a timer.** Its SPI transport blocks on
  `portMAX_DELAY` throughout; there is no 100 ms anywhere in it.

So the delay is in the radio path or the air, and it cannot be decomposed
further from this end. What would decompose it is the co-processor's own
instrumentation: `CONFIG_ESP_PKT_STATS` and the function profiling in
`esp/fruitjam-c6/`, which are two lines of sdkconfig and a reflash -- and
reflashing that chip is routine now.

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
It goes on over BOOTSEL and does not involve UbiqOS at all, so it is still the
way back when it is UbiqOS's own passthrough that is broken. It is deliberately
NOT committed here -- it is 185 kB of somebody else's binary and .gitignore
says so -- and lives in tools/ on the development machine.
