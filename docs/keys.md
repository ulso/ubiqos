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

`key lock` seals it again, `key remove NAME` forgets one key, and
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
