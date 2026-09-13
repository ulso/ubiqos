# TinyUSB, with one file changed

The Pico SDK's TinyUSB is used as it ships, except for `class/cdc/cdc_host.c`,
which is copied here with a single change. `CMakeLists.txt` lists the host
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

## Re-syncing after an SDK upgrade

    diff ~/.pico-sdk/sdk/<version>/lib/tinyusb/src/class/cdc/cdc_host.c \
         lib/tinyusb-patched/class/cdc/cdc_host.c

Everything outside `cdch_xfer_cb` and the one line that clears the retry count
should be identical. If upstream has fixed the TODO, delete this directory and
put the path in CMakeLists.txt back.
