# Keys: what the board keeps secret

An API key has to live somewhere. `/sd/config.txt` holds the WiFi password in
clear text, and a card gets handed around; an argument is in a process's memory
and in the shell's history. So UbiqOS keeps named secrets in flash, sealed with
a passphrase.

```
ubiqos:/> key unlock
no store yet: what you type now becomes the passphrase.
passphrase:                 (not shown)
working
a new store, unlocked

ubiqos:/> key set typesafe.api
value:                      (not shown)
stored

ubiqos:/> key
name                      bytes
typesafe.api              48

ubiqos:/> key check typesafe.api
fingerprint 0x2e0a1877
```

`key lock` seals it again, `key remove NAME` forgets one key (see
[Taking one out](#taking-one-out) for what "forgets" is worth), and
`key destroy` erases the store, passphrase and all.

## What it protects, and what it does not

- **A board that is switched off gives up nothing.** The store is sealed, and
  the key that opens it comes from the passphrase and is kept only in SRAM.
- **The card is no longer where the secrets are.** Nor is the shell's history,
  nor `ps`, nor any log: a value is typed at a console, never echoed, never an
  argument, and there is no call that returns one -- no `key get`.
- **A board left running and unlocked gives up everything.** UbiqOS has no
  memory protection, so any module can read any memory. While the store is
  open, its contents are in SRAM and reachable. This is a fact about the
  system, not a detail of the key store.
- **Somebody who takes the flash away can guess at leisure.** The iterations
  below buy a factor against that, not safety. What makes a store hard to open
  is the length of the passphrase.
- **Revocation still matters more than any of this.** A key you can withdraw
  at the service that issued it is worth more than one that is well hidden.

## How it is sealed

- **ChaCha20-Poly1305** over the records, which authenticates as well as
  encrypts: a wrong passphrase and an edited store give the same answer, no.
- **The key** is PBKDF2-HMAC-SHA256 over the passphrase, salted with sixteen
  random bytes kept with the store *and the chip's own unique id* -- so the
  same passphrase on two boards gives two different keys. The id is a serial
  number, readable by anyone holding the board: it is salt, never key.
- **25000 iterations**, which is about two seconds on this chip using its
  hardware SHA-256. The number is stored with each copy, so raising it later
  leaves older stores openable.
- **The cryptography is in the kernel** ([`kernel/crypto.c`](../kernel/crypto.c)),
  because the kernel runs from SRAM where mbedTLS does not fit. It is checked
  against the published test vectors by compiling that same file on a desktop.

## Where it lives

Two 4 kB sectors at `0x10FE0000`, written alternately with a sequence number,
so losing power in the middle of a write leaves the older copy whole. They sit
past the application region, so nothing a system update writes goes near them
-- and deliberately not in the last sector, which the RP2350-E10 block in every
UF2 erases.

Writing means erasing, which holds the flash for tens of milliseconds with
nothing readable from it. So it happens in the filesystem server rather than
in a system call, core 1 parks in a RAM loop first, and the new image is built
in SRAM -- never in PSRAM, which shares the QMI bus with the flash.

## Taking one out

`key remove NAME` takes a single key out. There is nothing partial about it
afterwards -- the name is gone from the listing, `key check` says there is no
such key, and it is still gone after a power cut, because what happens is not
an edit but a rewrite: the slot is cleared in SRAM and the whole store is
sealed again into the OTHER of the two sectors, which then becomes the live
one. Nothing can be edited in place here; the names are inside the ciphertext
with the values. If the write fails, the key goes back into the SRAM copy, so
the listing and the flash cannot end up saying different things.

**The sector that was live still holds the older sealed copy, key and all.**
It is ciphertext and the passphrase is what opens it, so this is not a hole so
much as a lifetime: it lasts until the next write, which erases that sector
before using it. Any write will do -- another `key set`, another `key remove`.
So a key that has to be gone from the chip rather than merely gone from the
store is gone after the next write, and `key destroy` erases both sectors at
once.

Nothing stops one module from removing a key that another put there. There is
no owner recorded and no permission asked: `key remove` is `key set` with a
length of zero, and while there is no memory protection a module that wanted to
could do it anyway. It is the same fact as the one above about an unlocked
board, seen from the writing side.

## The WiFi password

A password kept under **`wifi.<network name>`** is what `wifi connect` uses:

```
ubiqos:/> key unlock
ubiqos:/> key set wifi.my-network
value:
stored
ubiqos:/> wifi connect my-network
using the password kept for this network
joining.....
joined
```

The password goes from the store to the radio's driver inside the kernel. The
`wifi` command asks for the network by name and never sees the bytes -- which
is what "the kernel uses the key on your behalf" means, and it is the shape
anything else the kernel can do for itself should take.

**`wifi auto`** does not even ask for the name. It looks around, and of the
networks that are actually in earshot it joins the strongest one there is a
key for -- so one board can be carried between places and find whichever of
them it is in:

```
ubiqos:/> key unlock
ubiqos:/> wifi auto
looking around...
trying the-workshop...
joined
```

**Unlocking does this by itself** when the store holds a password for a
network. That is what the store was opened for: the clock wants the network,
and so does anything that calls out, and a board sitting there with the right
key and no link is the failure this avoids. `key unlock` runs the `wifi auto`
command afterwards -- the command, as you would have typed it, with its own
output on the screen. No radio code went into the key program, nothing links
the two, and a store with none of these keys leaves the radio alone.

The looking is what makes it quick: the networks that are somewhere else are
not tried at all, and a whole store of keys costs one scan of about three
seconds. `wifi scan` shows the same list, with a mark against the ones there
is a key for. A hidden network broadcasts no name and is in no scan, so that
one is still `wifi connect <ssid>`.

With the store locked, or with no key for that network, `wifi connect` asks at
the keyboard as it always has. `/sd/config.txt` is unchanged and is still what
makes a board join by itself at boot: the store is sealed then, and nothing can
open it until somebody types the passphrase. A board that must come back on its
own after a power cut therefore still keeps its password on the card.

## Two things the kernel does with a key

**`key match`** -- not a command, a call: a program hands in a name and a
candidate and gets back yes or no. It is how `sshd` checks a password without
ever being given the password to check against, and the comparison looks at
every byte whatever the first one says.

**`key derive`** -- thirty-two bytes that belong to this board and a label of
the caller's choosing, being HMAC over the key that opens the store. Nothing
stored is revealed and nothing has to be stored: the same label gives the same
answer for as long as the passphrase lives, and a different answer on any other
board. `sshd` gets its host key this way, because signing is elliptic-curve
arithmetic that does not fit in this kernel and a key it could read back would
be a key the store had handed over. Change the passphrase and every derived key
changes with it -- for a host key that means clients will notice.

## Using a key

The kernel reads the values; programs do not. Where the kernel can do the work
-- joining a WiFi network, computing a signature -- the key never leaves it.
An ordinary API key that has to be sent as it is will have to be handed to the
one module that sends it, and while there is no memory protection that is a
rule rather than a wall. It is still worth having: it keeps the key off the
card, out of arguments and out of logs, which is where keys are actually lost.

## What comes next

RP2350 has OTP that can be locked, and a secure mode. A key held there is what
would let a board unlock itself without somebody typing a passphrase -- the
thing a machine that must come back after a power cut needs. That is a larger
piece of work, and it only means anything with the debug port closed as well.
The format leaves room for it: the sealed copy carries its own parameters, so
where the key comes from can change without the records changing.
