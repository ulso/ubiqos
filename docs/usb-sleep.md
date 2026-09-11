# The network over USB does not come back after the Mac sleeps

Every project on this bench that carries CDC-NCM has the same complaint: the
Mac sleeps, the Mac wakes, and the USB network interface does not come back by
itself.

The easy answer is that macOS is at fault. It is also the suspect one, and for
a reason Ulf raised himself: **USB ethernet dongles speak the same protocol and
survive sleep.** If NCM were broken across the board on macOS it would be a
well-known scandal rather than a private annoyance. What is actually common to
all of these projects is not the host -- it is TinyUSB and our own descriptors.
There has already been one macOS-specific NCM descriptor bug on this bench, and
it was one byte.

## What this end could say about it: nothing at all

Before any of that can be argued, the device has to be able to say whether it
even noticed the host go away. It could not:

    tud_suspend_cb     absent
    tud_resume_cb      absent
    tud_mount_cb       absent
    tud_umount_cb      absent
    netif_set_link_up  called once at boot and never taken down

So the link was pinned up from startup whatever the bus did. myrtos could not
notice a suspend, could not recover from one, and could not report one. That is
the gap this closes -- the NOTICING. Recovering comes after, and only once the
log says what actually happens.

`usbstat` now reports:

    host suspends:  0
    host resumes:   0
    host mounts:    1
    host unmounts:  0
    last one at ms: 1791

and the first dozen of each event go into the log with a timestamp. After that
they are counted and not printed, because a host that suspends whenever the bus
idles would otherwise push the boot messages out of the ring -- and those are
the other thing a morning-after reading wants.

## The experiment, in two halves

**Half one, as it stands.** The board is on a CalDigit TS5, which keeps it
powered while the Mac sleeps. So it runs all night and the counters are a real
record of what the bus did. Sleep the Mac, wake it, and read `usbstat` and
`/var/dmesg`.

- Suspends and resumes counted, network still dead → the host came back and we
  did not. That is ours to fix, and the next question is whether the
  NETWORK_CONNECTION notification is re-sent after resume.
- Nothing counted at all → the host never told us, and the argument moves to
  the descriptors or to macOS.

**Half two, Ulf's idea.** Plug the board straight into the Mac instead of
through the hub. A port that powers down takes the board with it, and then an
empty log means something different from an empty log on the hub -- which is
exactly why the two halves have to be run separately and read separately.

## The other thing that happened that night

The board was found unresponsive to the keyboard in the morning, with the
display still running. That is recorded separately and is NOT attributed: see
the note in the commit of 11 Sep 2026. The short version is that a normal
picture means core 0 was alive and taking interrupts -- the character generator
renders from a timer interrupt, so a stopped CPU would show a repeating
48-line band rather than a picture -- which rules out the debug probe having
halted anything, and leaves starvation on core 0 as the shape that fits.

The test that would separate it in five seconds: there are two shells, one on
the screen and one on the serial port. If the serial one answers, core 0 is not
starved and the fault is in the keyboard path.
