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

## The fix, applied 11 September 2026

The font moved into SRAM, in the chargen builds only.

Two of the options below turned out to combine. Paying only where the danger is
costs nothing in a framebuffer build -- `chargen.c` IS the interrupt, and it is
compiled nowhere else, so a framebuffer build goes on reading the same table
from `console.c` in thread context where flash is as safe as it ever was. That
left riscv chargen short by rather less than the whole 3584, and the module pool
paid the rest: 124 kB -> 120, the same lever that had already given 4 kB to the
wall clock that morning. That pool has room and this heap has none.

Verified by address rather than by hope:

| build | font8x16 | font6x12 |
|---|---|---|
| arm chargen | `0x2002beb8` (SRAM) | `0x100001ec` (flash) |
| arm framebuffer | `0x10000c6c` (flash) | `0x100001ec` (flash) |
| riscv chargen | `0x20034018` (SRAM) | `0x100000f4` (flash) |
| riscv framebuffer | `0x10000b74` (flash) | `0x100000f4` (flash) |

`font6x12` stays in flash in every build: `console.c` reads it from thread
context and nothing else reads it at all. Asking who reads what is what settled
both halves of this.

### What was NOT verified, and why it is worth saying

A console flood was tried as a test and has no power to confirm anything. Two
thousand back-to-back `free` commands left the board pumping video at its normal
1461 a second -- and so did the SAME flood against a control build with the font
put back in flash. A test that both versions pass tests nothing.

So this fix rests on the diagnosis and not on a reproduction: the program
counter sat on `g[gy]` four separate times, the mechanism is a documented
property of the QMI, and the addresses above say the table moved. That is good
evidence and it is not proof.

What the flood is missing is most likely the other half of the original: the
keyboard re-enumerating on core 1 throughout, which the trigger produced and a
serial flood cannot. And the fault showed four times in two DAYS of use, so a
thirty-six second run samples a race far too narrow for it either way.

The verification is therefore time: the board not stopping again. Until then
this is an applied fix with a sound reason, not a demonstrated one.

The options as they stood, for the record:

* **Only in the chargen builds** -- taken, and it was not enough on its own.
* **Find the bytes elsewhere in the riscv builds** -- taken, from the module
  pool.
* **Keep drivers out of PSRAM**, a much larger change that gives back the reason
  modules are cheap. Not taken, and not needed for this.

## What is still open

The **key repeat that outlives the keyboard** is untouched. It is the trigger:
the keyboard drops, the repeat goes on sending Enter, the shell re-runs its last
line, and the flood is what gave the video pump enough work to sit in a font
read. The QMI stall is what turned that flood into a stopped machine, and that
half is now fixed -- a flood should cost frames and not the board. The repeat
loop will still flood.

So this document's fourth fault is half closed. Two faults were stacked; one of
them is gone.
