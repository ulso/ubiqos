# netcon: the shell over TCP

The board has had a console on a screen, on the USB serial port and over
Bluetooth, and none of them over the network it has been serving web pages on
since August. `netcon` is that console.

```
ubiqos:/> netcon &
netcon: a shell on port 23, to the USB cable only
```

and from the computer at the other end of the cable:

```
$ nc 192.168.7.1 23

ubiqos shell ready. Type 'help'.
ubiqos:/> free
SRAM  largest free: 38084 bytes
PSRAM largest free: 7578348 bytes
```

## What it is not

**It is not encrypted and it asks for no password.** Whoever reaches the port
gets a shell, and every keystroke crosses the network as it was typed.

That is why the default is the USB cable and nothing else. `192.168.7.0/24` is
a wire between two machines with nobody else on it, and a shell there is worth
about what a serial cable is worth. `netcon -a` opens the same shell to
everyone on the WiFi, which is a decision rather than a convenience. Anything
else on that network gets a prompt on the board:

```
$ nc 192.168.68.61 23
netcon: this shell answers on the USB cable only.
        Start it with -a to open it to the network.
```

SSH is what this is a step towards. The cryptography is a piece of work on its
own; the part that is finished here is everything else -- and that part turned
out to be the interesting half.

## How it works, which is barely at all

`netcon` does not implement a shell, a terminal or a device. It makes two
pipes, puts them on descriptors 0 and 1, starts the ordinary `sh`, and carries
bytes between those pipes and the socket:

    socket --> pipe --> sh's stdin
    socket <-- pipe <-- sh's stdout

A process started with a pipe on descriptor 0 cannot tell it from a serial
port, which is the whole reason this is three hundred lines. The shell's
line editing, history, Ctrl-C and prompt are its own and were not touched.

The session ends when the client closes the connection: `netcon` closes the
shell's input, an empty pipe with no writer reads as the end of the file, and
`sh` leaves. Only a shell that will not take the hint is killed.

One connection at a time. The next client waits for the socket rather than
being refused, and gets a fresh shell.

## One at a time, and how that looks

`netcon` serves one connection and then goes back to waiting. A client that
knocks while another is being served is accepted by the stack and waits its
turn, so what it sees is a connection that stays silent for a while -- not a
refusal, and not a failure.

That is worth knowing because it looks like a leak. Ten connections half a
second apart, each given a second and a half to say something, and the later
ones appear to fail: the board is simply still tidying up the one before.
Given time between them, `free` shows every ring and every socket handed back:

```
ubiqos:/> free
Pipe rings in use:  0 of 8
Sockets in use:     2 of 8
  socket 0: pid 10, port 80
  socket 1: pid 8, port 23
```

Those lines are new, and they exist because this was worth measuring rather
than arguing about. Both tables hold eight, and a service that cannot start
because one is full says nothing about why.

## The kernel bug it found

The first working version answered on the network and took its keystrokes
**from the serial port**. Output went to the right place; input came from the
wrong one.

`dup(fd, -1)` means "the lowest free descriptor", and the search for one asked
whether a slot had a device and whether it had a file. A slot holding a pipe
end has neither, so the pipe's own descriptor counted as free: `netcon` made
its two pipes, saved its stdin, and the save landed on top of the pipe. The
child then inherited the console.

Nothing said anything, which is the part worth remembering. The shell can
reach the same bug with a pipeline that also redirects. It is
`kernel/io.c:is_free` now, in one place instead of two.

## Telnet

A telnet client announces itself with IAC (255); a raw client such as `nc`
sends nothing. So the negotiation is answered and never offered -- replying to
bytes that arrived cannot put line noise on the screen of a client that does
not speak telnet.

What is worth negotiating is the other end's line editing. Left alone, a
telnet client collects a whole line, echoes it itself and sends it on return:
no Ctrl-C, no arrow keys, and every character twice. `WILL ECHO` and
`WILL SUPPRESS-GO-AHEAD` are what turn that off, and they go out as soon as
the client says IAC.

With `nc` the same is done at your own end:

```
stty -icanon -echo; nc 192.168.7.1 23; stty sane
```

Without it `nc` sends a line at a time, which works but has no line editing
and no Ctrl-C.
