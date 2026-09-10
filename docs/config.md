# /sd/config.txt

What this machine is called, and what network it belongs to. Read once at boot,
by the filesystem server, as soon as the card is up and before `/sd/startup`
runs.

    # myrtos
    hostname = jamboree
    ssid     = the-network
    password = ...

Keys are case-insensitive, `#` starts a comment, and a value runs to the end of
the line with the spaces either side trimmed. No file, or no card, and nothing
is lost: the machine is called `myrtos`, as it always has been.

A hostname is also an mDNS label, so it may hold letters, digits and hyphens
and nothing else, and may not begin or end with one. Anything else is said out
loud and ignored rather than announced and refused later by the responder.

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
