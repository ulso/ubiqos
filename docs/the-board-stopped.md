# The board stops with the picture still on the screen

Three times in two days: the keyboard dead, USB gone, neither network
answering -- and a perfectly normal display.

## What the display proves, and what it does not

It proves the processor is alive. The character generator has no framebuffer:
48 scanline buffers are refilled by a TIMER INTERRUPT just ahead of the beam
(`kernel/video.c`, `pump_isr`). A stopped CPU would leave the DMA cycling the
same 48 buffers, and the screen would show one 48-line band repeated down the
page. A normal picture means interrupts are being served.

It proves nothing about the threads. The USB device stack, both lwIP
interfaces, the socket server and the drain that collects keystrokes from core
1 all live in ONE thread, and that thread dying takes all four with it while
the display carries on. That is the signature exactly.

## The five-second test

There are two shells: one on the screen and one on the serial port. If the
serial one answers, the scheduler is fine and the fault is in the keyboard
path. If neither does, it is deeper. `ps` then says which threads are left.

## How the answer was actually found

The crash record existed already -- `kernel/crashlog.c`, written for a probe to
read by symbol -- and was useless twice, because the way to get the board back
is BOOTSEL and BOOTSEL wipes the RAM it lives in. **The evidence was destroyed
by the act of recovering the machine.**

Two changes fixed that:

* `myrtos_crash` now lives in `__uninitialized_ram`, which survives a warm
  reset, and `myrtos_crash_report` puts it in the log at the next boot. A power
  cycle still loses it, and that is the honest limit.
* The Debug Probe can flash this board directly, so a wedged machine can be
  recovered WITHOUT BOOTSEL and without losing the record:

      openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
              -c "program build/os_kernel.elf verify reset exit"

  and reading the record off a board still sitting in the fault is

      openocd ... -c "init" -c "halt" -c "mdw 0x20000110 4"

  where the address is whatever `nm build/os_kernel.elf | grep myrtos_crash`
  says today.

## What it was, on 11 September 2026

    0x20000110: 43524148 00000001 2002930c 2001a439
                "CRAH"   PANIC     fmt       caller

Kind 1 is a panic, and the SDK's panics are all string literals, so the pointer
names the caller without decoding anything. At `0x2002930c`:

    "sys_timeout: timeout != NULL, pool MEMP_NUM_SYS_TIMEOUT is empty"

and `0x2001a439` is inside `sys_timeout_abs`, `timeouts.c:190`.

**lwIP ran out of timer slots.** `MEMP_NUM_SYS_TIMEOUT` was 12, chosen when
there was one network interface. Counting what wants one now: TCP 1, ARP 1,
DHCP 2, AutoIP 1, IGMP 1, DNS 1 -- seven before any application -- and then the
mDNS responder, which takes several per interface while it probes and
announces, on two interfaces. Twelve was just under, so the board ran for
minutes and then died the moment something asked for one more.

Running out is not a dropped timer. lwIP asserts, and an assert is a panic.

It is 24 now. Each slot is a dozen bytes.

## Verified, not hoped

A script that runs `browse` and `ping` in a loop killed the board in its first
round before the change, and survived ten rounds -- sixty commands -- after it.

Two other changes were made at the same time on suspicion and are NOT known to
have fixed anything: the mDNS querier's name buffers became static rather than
sitting on that thread's stack, and the USB task's stack went from 4 kB to 6.
Both are defensible on their own -- that one thread now carries far more than
4 kB was measured against -- but the crash record named the timeout pool, and
only that.
