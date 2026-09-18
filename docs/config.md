# /sd/config.txt

What this machine is called, and what network it belongs to. Read once at boot,
by the filesystem server, as soon as the card is up and before `/sd/startup`
runs.

    # myrtos
    hostname = jamboree
    ssid     = the-network
    password = ...

Keys are case-insensitive and a value runs to the end of the line with the
spaces either side trimmed.

A `#` starts a comment when it begins the line or follows a blank, and not when
it sits in the middle of a word -- so `password = se#cret` keeps its hash and
`ssid = home # the one downstairs` still ends at it. That distinction is not
pedantry: any `#` used to end the line, so a password containing one was
silently cut short and the only symptom was a network that would not join.
A password may now hold `#` anywhere except directly after a space, which is
the whole of what is left of the trade. No file, or no card, and nothing
is lost: the machine is called `myrtos`, as it always has been.

A hostname is also an mDNS label, so it may hold letters, digits and hyphens
and nothing else, and may not begin or end with one. Anything else is said out
loud and ignored rather than announced and refused later by the responder.

## The USB cable's address

    usb_address = 192.168.7.1

The board's address on the network over its USB cable, and the default when
the file does not say. The computer at the other end is offered the next
address up by the board's own DHCP server, on a /24 -- 192.168.7.2 here -- with
no router and no DNS server, so it never tries to reach the internet through
the board. The name still works: `hostname.local` answers on the cable as on
the WiFi.

Choose one that is not on any network the computer already has. The last
number must be 1 to 253, to leave room for the computer's; loopback, multicast
and 169.254 are refused, with a line saying so, and the default kept.

It was 169.254, from AutoIP, until 18 Sep 2026. On a computer with more than
one network that address range is routed out of one of the others, and the
board could not be reached on its own cable -- see `kernel/lwipdhcpd.c`.

## When it is read, and why that is the whole of it

The filesystem server reads it as soon as the card is up and before
`/sd/startup` runs, and lwIP does not start until it has. That ordering is not
tidiness: mDNS announces a name ONCE and cannot unsay it, so a stack that comes
up before the card has been read answers to the wrong name for ever.

The flag that says the card has been looked at is set AFTER the reading, not
before. It was before -- so that an early return still counted as having
tried -- and reading the card takes milliseconds during which the USB task ran,
saw the flag, and started lwIP with a hostname nobody had read yet. The log
showed `answering to myrtos.local` three lines above `config: hostname
jamboree`. It was right the first time it was tested, which is how a race that
usually wins survives.

## The password

`wifi connect` has always taken the password from the keyboard: never echoed,
never an argument, never over the serial port. A file could undo all of that in
one `cat`, so it does not:

* the kernel reads config.txt itself, through the FAT library, and hands the
  bytes to nothing;
* the filesystem server refuses to open or read that one path for any process.
  `cat` says "this one is not readable" -- a refusal has an answer of its own,
  `MYRTOS_FS_REFUSED`, precisely so it is not confused with a missing file;
* writing is still allowed, so an editor can replace it, and so is `rm`;
* `wifi connect` with no arguments joins the network named there. The kernel
  does the joining, so the module that asked never sees the password.

`usbdisk` is the hole that remains, and it is left open deliberately: it hands
the whole card to a host as a block device, which is what it is for, and
anybody who can type that command can put the card in a reader anyway.

## What is asked for, and where

If the file names a network but no password, `wifi connect` asks for the
password. If it names neither, it asks for both. **It asks wherever it is run
-- and that is the whole of the answer to "what about a build with no video".**

Asking at boot would need a console, and a build without video has no keyboard
of its own; a boot that stops to ask a question nobody can answer is worse than
one that says what it does not know. So boot never asks. It reads the card,
says what it found -- 

    config: hostname jamboree, a network but no password

-- and leaves it there. `wifi connect` in `/sd/startup` is not the way to fill
the gap either: that shell's standard input is the script, so a prompt would
read the next line of the file. Put both settings in config.txt for a machine
that should connect by itself, and type `wifi connect` at whatever console you
have for a machine that should not.
