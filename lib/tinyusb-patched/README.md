# TinyUSB, with two files changed

The Pico SDK's TinyUSB is used as it ships, except for two files copied here,
each with a single change: `class/cdc/cdc_host.c` and `host/hub.c`. `CMakeLists.txt` lists the host
sources by path, so pointing one of them at this directory is the whole of the
mechanism -- there is no patch step and nothing to remember to apply.

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

In its place: a failed IN transfer is queued again, at most `MYRTOS_RX_RETRY`
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

In its place: re-arm on failure, bounded at `MYRTOS_HUB_RETRY` **consecutive**
failures, with any success resetting the count. Re-arming here is what an
earlier attempt to do it from outside could not — that one was refused because
the endpoint was still busy, asserted, and was reverted. In this callback the
transfer has just completed, so the endpoint is free, and the call is the same
one the file already makes two cases further down.

## Re-syncing after an SDK upgrade

    SDK=~/.pico-sdk/sdk/<version>/lib/tinyusb/src
    diff $SDK/class/cdc/cdc_host.c lib/tinyusb-patched/class/cdc/cdc_host.c
    diff $SDK/host/hub.c           lib/tinyusb-patched/host/hub.c

Everything outside the two callbacks named above should be identical. If
upstream has fixed either one, delete that copy and put the path in
CMakeLists.txt back.
