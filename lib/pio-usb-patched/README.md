# Pico-PIO-USB, with one line added

`pio_usb.c` is the submodule's `lib/Pico-PIO-USB/src/pio_usb.c` with one line
added in `pio_usb_bus_prepare_receive`, marked `UBIQOS:`. `CMakeLists.txt`
compiles this copy instead of the submodule's, and stops the configure if the
submodule's copy is no longer the one this was made from.

## What it fixes

Pulling one device out of the Fruit Jam's onboard hub took the other devices
on that hub with it: the BleuIO went silent, the hub stopped reporting ports,
and only a reset brought them back. Measured 26 Nov 2026, with every failed
transaction logged by frame:

* The frame Keybridge was pulled, its interrupt IN got no answer at all. That
  is expected.
* From the next frame on, every answer from the BleuIO and from the hub
  arrived **one bit late**. The BleuIO's NAK came in as SYNC `0x01`, PID
  `0xB5` instead of `0x80`, `0x5A`. The hub's status reply, the one carrying
  the news that a port had changed, came in as PID `0x87` instead of DATA0
  `0xC3`. In each case it was the right packet with one `1` bit in front.
* Three bad answers in a row end a transfer. The hub's status endpoint and
  the BleuIO's bulk IN gave up, so the removal was never reported, and the
  host went on asking for the missing device every frame. That kept the fault
  going for good.

The receiver is two state machines. The edge detector finds the packet and
raises `DECODER_TRIGGER` once per bit, and the NRZI decoder waits on that
trigger and shifts one bit per trigger. After a transaction with **no
answer**, the edge detector is left waiting for a packet, and the next packet
on the bus is the host's own SOF. It decodes that and raises triggers while
the decoder is switched off, and the last trigger stays pending. Then
`pio_usb_bus_prepare_receive` restarts the decoder and enables it, and the
decoder takes that trigger at once and samples the idle bus as a `1`. Every
byte of the next packet is then one bit late. A transaction that is answered
leaves the edge detector waiting at its end-of-packet step, where it ignores
the SOF, so the trigger is only left over after a missing answer.

The line added clears `DECODER_TRIGGER` while the decoder is still off.

After the change, the same pull showed fifteen unanswered polls of the
missing device and nothing else. The hub then reported the removal, TinyUSB
said `device 2 removed`, and the BleuIO went on undisturbed.

## When the submodule moves

    diff lib/Pico-PIO-USB/src/pio_usb.c lib/pio-usb-patched/pio_usb.c

Everything outside the line marked `UBIQOS:` should be identical. Copy the new
file here, add the line again, and put the new file's
`shasum -a 256 lib/Pico-PIO-USB/src/pio_usb.c` into `CMakeLists.txt`. If
upstream has fixed it, delete this directory and point `CMakeLists.txt` back
at the submodule.
