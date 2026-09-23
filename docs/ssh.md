# SSH

A shell on the board, over the network, encrypted and behind a password.

```
$ ssh ulf@192.168.7.1
ulf@192.168.7.1's password:

ubiqos shell ready. Type 'help'.
ubiqos:/> ls
dev             <dir>
var             <dir>
```

```
ubiqos:/> key set ssh.password
value:
stored
ubiqos:/> key unlock
passphrase:
working
unlocked
sshd: listening on port 22
```

**Unlocking starts it**, when the store holds a password under `ssh.password`
and no sshd is running already. A locked board has neither the password nor
the host key, and unlocking is both the moment they appear and the moment
somebody is demonstrably present, which is the right condition for opening a
way in. `sshd &` by hand does the same thing on a board whose store was
unlocked earlier.

**A board with a key file starts it at boot**, because the store is opened
then with nobody there -- see [keys.md](keys.md#opening-at-boot-the-key-file).
That is a way in that opens without anybody present, and it is the reason the
key file is something to switch on rather than the default.

The key store must be unlocked, because both the password and the host key come
from it. A board that must answer SSH after a power cut therefore needs somebody
to type the passphrase first -- which is the same trade as everything else in
[keys.md](keys.md), and deliberate: a shell over the network should need a
decision.

## What it speaks

| | |
|---|---|
| key exchange | `curve25519-sha256` |
| host key | `ecdsa-sha2-nistp256` |
| cipher | `aes256-gcm@openssh.com`, both directions |
| authentication | `password`, checked in the kernel against the key store |
| channel | one session, one shell, with the pty the client asks for |

One of each, because a second choice is a second thing to get wrong and every
OpenSSH client made this decade offers all four.

**Not Ed25519**, which is what most people's keys are: mbedTLS has no Edwards
curves at all, so neither the host key nor a client key can be one. **Not
post-quantum**: OpenSSH now prefers `mlkem768x25519-sha256` and says so in a
warning; ML-KEM is not in mbedTLS either.

**Not audited.** The cryptography underneath is mbedTLS and the randomness is
the chip's own generator, but the protocol around them is ours. Put it on your
own network; do not put it on the internet.

## One client at a time

sshd serves one connection and then goes back for the next. While somebody is
logged in, a second `ssh` is accepted by the network stack and then hears
nothing -- no version line, no password prompt -- until the first session ends
or the second client gives up. That looks like a server that has hung, and it
is not one. An `ssh -N` master connection with no session open counts as
somebody logged in.

## The host key is derived, not stored

The store never hands a value back -- that is its whole promise -- so sshd
cannot keep a host key in it and read it out again. Instead the kernel derives
one: thirty-two bytes of HMAC over the key that opens the store, under the
label `ssh.hostkey`. The same board with the same passphrase gets the same host
key for ever, nothing is written to the card, and nothing stored is revealed.

`UBIQOS_KEY_OP_DERIVE` is that call, and `UBIQOS_KEY_OP_MATCH` is how the
password is checked: the candidate goes in, yes or no comes back, and sshd
never holds the real one.

**Change the passphrase and the host key changes with it**, and every client
will say the host key has changed. That is the price of not storing it.

## The shell is the ordinary shell

sshd implements no shell and no terminal. It asks the kernel for a two-way pipe
pair, gives one end to `sh` as descriptors 0, 1 and 2, and carries bytes
between the other end and the SSH channel. Everything the shell does -- line
editing, history, Ctrl-C, the prompt -- is its own.

Two things had to be true for that to work, and neither was at first:

**A terminal is bidirectional.** With one pipe each way, descriptor 2 could be
readable or writable but not both -- and `more` prints its prompt on descriptor
2 and reads the key from descriptor 2, on purpose, so that `cat x | more` does
not wait for the file to press a key. Over two pipes, `more` printed on the
board's own screen and waited for the board's own keyboard. `ubiqos_pipepair`
is the answer: two rings with their ends crossed, which is a socketpair, and
the nearest thing to a terminal that needs no new device.

**A newline is not a new line.** The console driver turns LF into CR LF; a pipe
does not. Output that looks right on the screen came out as a staircase over
the network until sshd did that translation itself, which is the one thing a
pty layer would have been needed for.

## When the window is resized

The client says so -- a `window-change` request with the new rows and columns
-- and there is no signal to send and no terminal driver to tell. So sshd puts
it into the shell's input **in band**, as exactly what a terminal answers when
asked its size: `ESC [ 8 ; rows ; cols t`. Everything downstream already knew
that sentence:

- the shell takes its width from it and redraws the line;
- curses turns a changed size into `KEY_RESIZE`, and Atto -- which had
  `resize-terminal` bound to it all along -- redraws to fit;
- `more` does not take it for a key and turn a page;
- the prompts that keep secrets drop it like any other escape sequence, which
  matters: before that filter existed, a resize during `key unlock` would have
  put `[8;40;130t` into the passphrase.

## What Ctrl-C does

What it does on the screen. sshd does not put the key into the shell's input:
it hands it to the kernel, which ends the command running in front -- a
`sleep 20000`, a long `fetch` -- the way the console driver does for the
board's own keyboard. A pipe pair has no driver of its own, so this is that
driver's one job done by the kernel instead.

When nothing is running in front, the key goes through as a key: at the
prompt the shell abandons the line, and `more` stops. A program that has taken
the key for itself -- Atto, through curses' `raw()` -- gets it too, which is
why `C-x C-c` and every other Emacs command with a Ctrl-C in it still work.

Closing the connection ends everything the session started, not only the
shell: an Atto left running in a window that was closed goes with it.

## What the debugging looked like

Worth writing down, because both wrong turns were the same mistake seen from
opposite sides.

The handshake completed, the client accepted the host key, and then said
`error in libcrypto` and hung up. A probe written here -- a minimal SSH client
in Python -- agreed: the signature did not verify. So the hunt was for a
difference in what the two sides hashed. There was none: the shared secret was
identical byte for byte, the exchange hash was identical, `d*G` on the board
matched `d*G` on the desktop, and mbedTLS verified its own signature. Two
thousand five hundred variations of the transcript were tried and none fitted.

**`ecdsa-sha2-nistp256` names a signature scheme, and a scheme includes its
hashing.** The message is the exchange hash H, and ECDSA hashes it before
signing: the signed digest is SHA-256(H). mbedTLS's `ecdsa_sign` takes a digest
that has already been made, so it must be given SHA-256(H) -- and it was being
given H. The signature was valid for a message nobody checks.

The Python probe made the mirror-image mistake: `cryptography`'s `verify()`
takes a *message* and hashes it, so feeding it H measured signatures over
SHA-256(H). Two errors that pointed at each other, and what broke the deadlock
was the one measurement that could not lie -- the board and the desktop
computing the same public key from the same private one.
