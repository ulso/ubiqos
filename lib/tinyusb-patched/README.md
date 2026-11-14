# TinyUSB, with four files changed

The Pico SDK's TinyUSB is used as it ships, except for four files copied here:
`class/cdc/cdc_host.c`, `host/hub.c` and
`portable/raspberrypi/rp2040/dcd_rp2040.c` with a single change each, and
`class/net/ncm_device.c` with three small ones. `CMakeLists.txt` lists the host
sources by path, so pointing one of them at this directory is the whole of the
mechanism -- there is no patch step and nothing to remember to apply. The two
device-side files are the exception: they come in with `tinyusb_device`, so the
SDK's copies are switched off with `HEADER_FILE_ONLY` beside the line that adds
ours. Were both compiled, the link would fail on a duplicate symbol -- that
failure is the check that the switch still works.

## What is changed, and why

`cdch_xfer_cb` opened with `TU_ASSERT(event == XFER_RESULT_SUCCESS)`, under
upstream's own `// TODO handle stall response, retry failed transfer`. Pulling a
CDC device out fails its outstanding IN transfer, and that assert then does two
harmful things:

* `TU_ASSERT` calls `TU_BREAKPOINT`, which on ARM executes `BKPT #0` whenever a
  debugger is attached. So an unplug **halts the core the USB host runs on** —
  measured on the Fruit Jam, 13 Sep 2026: core 1 stopped there with `DFSR`
  reading BKPT, which froze `TIMER0` through `DBGPAUSE` and stopped
  `time_us_64` with it. A power cycle was the only way back.
* With no debugger it returns before the line that queues the next transfer, so
  the dongle goes deaf and stays deaf. `tu_edpt_stream_read` queues the next
  read after every read, and nothing reads a stream with nothing queued.

In its place: a failed IN transfer is queued again, at most `UBIQOS_RX_RETRY`
times, and then left alone. A device that is really gone must not be asked for
ever — and once this returns instead of halting, the removal is noticed and
`cdch_close` tidies up. A transfer that lands resets the count, so an unplug
and a replug get the same patience as the first time.

`usbh.c` calls `xfer_cb` as a statement and ignores its return, so returning
true costs nothing.

## host/hub.c

`hub_xfer_cb` opened with `TU_VERIFY(result == XFER_RESULT_SUCCESS)`, which
returns before the interrupt poll is queued again — and that poll is the only
thing that ever tells this stack a device arrived or left. One failed transfer
and the hub is deaf for the rest of the run.

Measured on the Fruit Jam, 13 Sep 2026: pulling the keyboard left the hub's
endpoint 0x81 reading `NOTHING QUEUED` with three failures, and neither that
removal nor plugging it back in was reported at all. The keyboard was dead and
the board looked healthy.

In its place: re-arm on failure, bounded at `UBIQOS_HUB_RETRY` **consecutive**
failures, with any success resetting the count. Re-arming here is what an
earlier attempt to do it from outside could not — that one was refused because
the endpoint was still busy, asserted, and was reverted. In this callback the
transfer has just completed, so the endpoint is free, and the call is the same
one the file already makes two cases further down.

## Re-syncing after an SDK upgrade

`CMakeLists.txt` will not let you forget: it holds `ubiqosPatchedFrom` beside
the source list and **stops the configure** when that and `sdkVersion` differ.
So this is not a note anybody has to remember to read — it is what the error
tells you to come here for.

    SDK=~/.pico-sdk/sdk/<version>/lib/tinyusb/src
    diff $SDK/class/cdc/cdc_host.c  lib/tinyusb-patched/class/cdc/cdc_host.c
    diff $SDK/host/hub.c            lib/tinyusb-patched/host/hub.c
    diff $SDK/class/net/ncm_device.c lib/tinyusb-patched/class/net/ncm_device.c
    diff $SDK/portable/raspberrypi/rp2040/dcd_rp2040.c \
         lib/tinyusb-patched/portable/raspberrypi/rp2040/dcd_rp2040.c

Everything outside the places marked `UBIQOS:` should be identical. If
upstream has fixed one, delete that copy and put the path in CMakeLists.txt
back -- for the two device-side files, also take the SDK's path out of the
`HEADER_FILE_ONLY` line.

## class/net/ncm_device.c

A Mac that wakes from sleep behind a dock re-enumerates every board -- about
every seventeen minutes through a night of DarkWakes on macOS 27.2.1. Each
time, it selects the data interface's alternate setting 1, and now and then it
does so more than once in the same enumeration. Its own log shows each
selection as `AppleUSBNCMData::setupDataTransfers` and the result as
`updateLinkStatus`.

Upstream sends CONNECTION_SPEED_CHANGE and NETWORK_CONNECTION once per bus
reset and then sits in `NOTIFICATION_DONE`, with `notification_xmit_is_running`
left true for ever. After a second selection the Mac waits for
NETWORK_CONNECTION, gets nothing, and keeps the link down until the next
re-enumeration. Measured from the Mac's log on 12-13 Nov 2026, the Challenger+
behind a USB 2 hub on a CalDigit TS5 dock: **8 of 35 bring-ups ended with the
link down, and those 8 were exactly the ones with more than one selection.**

Three changes, each marked `UBIQOS:`:

* SET_INTERFACE to alternate setting 1 starts the notifications again from
  CONNECTION_SPEED_CHANGE, every time.
* `notification_xmit_is_running` goes false once the last notification has
  been delivered, so the next SET_INTERFACE is not turned away as busy. If a
  notification is still in flight when the host selects again, its completion
  carries on, because `netd_xfer_cb` asks for the next one.
* SET_ETHERNET_PACKET_FILTER (0x43) is accepted and ignored instead of
  stalled. `kernel/usb_descriptors.c` declares it in bmNetworkCapabilities,
  which macOS needs to see. The stall came back to the Mac as EIO on
  SIOCADDMULTI and SIOCDELMULTI, which is when it tore the data interface
  down and selected it again. embassy made the same request a no-op
  (embassy-rs/embassy#6844) and measured 0 failed attachments in 19 against
  macOS, against 3 in 9 before.

The second selection is what costs the link, and the first change makes it
harmless; the third should make it rarer. The Mac's log tells them apart:
count `setupDataTransfers` per bring-up, and look at the last `linkStatus`.

## portable/raspberrypi/rp2040/dcd_rp2040.c

When a Mac restarts, it takes the boards on its dock back and, as part of the
mass storage class's reset recovery, sends CLEAR_FEATURE(ENDPOINT_HALT) to the
mass storage OUT endpoint -- an endpoint that was never stalled and still has
the next CBW read queued in the controller.

`usbd_edpt_clear_stall` marks the endpoint idle whatever was queued, and then
hands the request to the class driver. `mscd_control_xfer_cb` sees an idle OUT
endpoint and queues the CBW read again. But upstream's `dcd_edpt_clear_stall`
cleared only the STALL bit, so the controller still held the first buffer,
and `_hw_endpoint_buffer_control_update32` stopped the board with
`panic("ep %02X was already available")`. It did so in the USB thread, which
is also where lwIP runs: the board left USB and the WiFi together, and stayed
gone until it was reset.

Seen on 14 Nov 2026 after a run of Mac restarts: the Fruit Jam and one
Challenger+ vanished, one Challenger+ came through. The Fruit Jam's backtrace,
read over its Debug Probe with the board still hung: `panic` <-
`_hw_endpoint_buffer_control_update32` <- `hw_endpoint_xfer_start` <-
`dcd_edpt_xfer` <- `usbd_edpt_xfer` <- `prepare_cbw` <- `mscd_control_xfer_cb`
<- `process_control_request` <- `tud_task`. Two boards had vanished the same
way after a Mac restart two days earlier; nothing was read from them before
they were reset, so whether it was this is not known, though it fits. A reset
by the RUN pin did not keep the crash record (`ubiqos_crash`) for the next
boot to report, so read a hung board over its probe before resetting it.

The change: `dcd_edpt_clear_stall` takes back a buffer the controller still
holds, with EP_ABORT in the same order `reset_ep0` already uses for endpoint
0, so the endpoint is as idle as `usbd` believes. Each takeback is counted,
and `usbstat` shows the count as `halt takebacks` -- a restart that the board
survives with the count still zero did not exercise this path.
