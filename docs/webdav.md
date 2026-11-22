# The card over WebDAV

`httpd` serves the SD card at `/dav/` over WebDAV, so a computer can mount
it and any editor can open and save files on the board while the board goes
on using the card. On a Mac: Finder, Cmd-K, `http://fruit-jam.local/dav/`.

## Password

`key set dav.password` on the board, typed at the `value:` prompt. Any user
name is accepted with it. HTTP Basic sends it in the clear, so it is its own
key and not `ssh.password`; only the kernel ever compares it. A wrong one
costs half a second. Without the key nothing but OPTIONS is answered.

## What it does

OPTIONS, PROPFIND (depth 0 and 1), GET, HEAD, PUT (by length or chunked,
with `Expect: 100-continue`), DELETE (a directory with what is in it),
MKCOL, MOVE, COPY of a file, PROPPATCH (accepted, nothing kept), LOCK and
UNLOCK. The locks are tokens and nothing more; Finder mounts a server
read-only unless it says it does class 2, and with one user that is honest.

A PUT writes `NAME.dav~` and renames it over `NAME` at the end, so an upload
that breaks off leaves the old file whole.

Every resource says it was modified when the board started (/var/time less
/var/uptime): the file system call gives no times out and the FAT library
stamps none. "Now" was tried first and was slow -- a client trusts no copy of
a file that changed a moment ago. The price of a fixed time: a file changed
on the board itself, keeping its size, is noticed at the next boot.

## Speed, 22 Nov 2026 (Fruit Jam, 240 MHz)

- Over the cable (http://192.168.7.1/dav/): 243 kB/s, a small file in
  about 10 ms on a kept connection. Over the WiFi: 125-155 kB/s.
- Finder reads whole files to draw thumbnails, and the card's root holds
  14 MB of sound files. An empty `.ql_disablethumbnails` (and
  `.ql_disablecache`) at the top of the card stops it for this volume.
- Files whose names begin with `._` that a Mac wrote while the card was
  mounted over USB are marked hidden on FAT: a listing leaves them out, a
  lookup by name still finds them.

macOS writes `.DS_Store` and `._NAME` files onto the card, as it does on any
network volume. `defaults write com.apple.desktopservices
DSDontWriteNetworkStores -bool true` stops the first kind.

`/tmp/dav.log` (memory, under 32 kB) has each request line with the headers
that decide anything, and each answer's status: `curl http://NAME.local/tmp/dav.log`.

## What it took, 22 Nov 2026

- **O_CREAT made nothing.** A file came into being at its first byte, so an
  empty file could not exist and every Finder copy (empty first, filled
  after) failed with error -36. O_CREAT creates now (kernel/fsserver.c,
  fat32lib's write of zero bytes).
- **Six TCP connections in all.** A mounted volume keeps four or five; the
  rest were reset. Twelve now, sixteen sockets, eight waiting per listener.
- **Kept-alive connections closed after 2 s,** and four slots with no giving
  way: Finder said the server was not responding, and one connection waited
  67 s. Quiet connections now stay 30 s, and a newcomer takes the slot of the
  one quiet longest.

Tried with Finder (copy, paste) and Zed (open, edit, save) on the Fruit Jam
at 960x540.
