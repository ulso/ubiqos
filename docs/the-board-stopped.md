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


# The fourth time, 11 September 2026 — a different fault, and an open one

The crash record was EMPTY. No panic, no trap, no assert: the board had not
crashed, it had stopped. That is a different thing and needed different tools.

## What the probe said

    myrtos_ticks        213733 -> 217831   over four seconds
    myrtos_video_pumps  275951 -> 275951   frozen
    myrtos_video_lines  frozen
    pc                  0x200088b2

The millisecond tick was still counting, so the machine was not dead. The video
pump was not, and the program counter was inside `myrtos_chargen_band` at
`chargen.c:349` -- which is this line:

    const uint8_t *g = myrtos_font8x16[ch];

**The font is in flash.** The character generator reads it from a TIMER
INTERRUPT, and since 10 September this machine also runs driver modules whose
code lives in PSRAM. Flash and PSRAM share one QMI. An interrupt that lands in
the middle of a PSRAM instruction fetch and then reads flash waits on a bus
that cannot serve it. OpenOCD agreed, in its way: "target was in unknown state
when halt was requested".

Everything else follows. The pump never returns, so nothing at thread level
runs again -- no USB, no network, no keyboard drain. The tick keeps counting
because it is a higher priority and touches neither flash nor PSRAM. And the
picture stays on the screen because the DMA goes on cycling buffers that nobody
is refilling.

That is the signature that made three days of this confusing: **a normal
display, a dead everything else, and no crash record.**

## And what set it off

A photograph of the screen, which no amount of probing would have given:

    Processes alive: 9
    USB host: keyboard ready
    USB host:   armed

repeated down the whole screen. `Processes alive` is the `free` command, so the
shell was running `free` over and over while the keyboard re-enumerated between
each. myrtos has a note about exactly this from before -- "key repeat outlives
the keyboard, repeat_key held 0x28" -- and 0x28 is Enter.

So the keyboard dropped, the repeat kept sending Enter, the shell kept
re-running the last line, and the console flood is what gave the video pump
enough work to sit in the font read while a module was executing from PSRAM.

Two faults, stacked. The repeat loop is the trigger and the QMI stall is what
turns a flood into a stopped machine.

## The fix, and why it is not applied

Move the font into SRAM. It is 3584 bytes and it is not affordable everywhere:
it took the riscv framebuffer build's C heap from 4972 bytes to 876, and the
riscv chargen build below its 4096-byte floor. Both stopped linking.

The options, none of which is mine to choose:

* **Only in the chargen builds.** The framebuffer console draws from thread
  context, where a stall is a wait and not a deadlock, so it does not need it.
  Still leaves riscv chargen 1188 bytes short.
* **Find the 3584 bytes elsewhere in the riscv builds**, which have been the
  tight ones all along.
* **Keep drivers out of PSRAM**, which is a much larger change and gives back
  the reason modules are cheap.

`font6x12` stays in flash either way: kernel/console.c reads it from thread
context, and asking who reads what is what settled that.
