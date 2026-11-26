#!/usr/bin/env python3
"""Drive Blixt in a pseudo-terminal and show what is on its screen.

A small VT emulator: cursor moves, SGR colours, erase, and an answer to
ESC [ 18 t, which is all Blixt sends. Keys are given as a list of strings;
"<F9>", "<ESC>", "<UP>" and friends are spelled out.

  drive.py BLIXT [-r ROWS] [-c COLS] [--file F] -- KEY ...
  drive.py BLIXT -- "<F4>" "hello" "<F2>"

Each "|" in the key list prints the screen at that point; one is printed at
the end anyway. --attrs prints colour names for the cells of one row.
"""
import argparse, os, pty, select, sys, time, re

KEYS = {
    "<ESC>": b"\x1b", "<ENTER>": b"\r", "<TAB>": b"\t", "<BS>": b"\x7f",
    "<UP>": b"\x1b[A", "<DOWN>": b"\x1b[B", "<RIGHT>": b"\x1b[C", "<LEFT>": b"\x1b[D",
    "<HOME>": b"\x1b[H", "<END>": b"\x1b[F", "<PGUP>": b"\x1b[5~", "<PGDN>": b"\x1b[6~",
    "<DEL>": b"\x1b[3~", "<F1>": b"\x1bOP", "<F2>": b"\x1bOQ", "<F3>": b"\x1bOR",
    "<F4>": b"\x1bOS", "<F6>": b"\x1b[17~", "<F9>": b"\x1b[20~", "<F10>": b"\x1b[21~",
    "<ALTF3>": b"\x1b[13;3~", "<ALTF9>": b"\x1b[20;3~",
}
for c in "abcdefghijklmnopqrstuvwxyz":
    KEYS["<ALT%s>" % c.upper()] = b"\x1b" + c.encode()
    KEYS["<C%s>" % c.upper()] = bytes([ord(c) - 96])
for c in "0123456789":
    KEYS["<ALT%s>" % c] = b"\x1b" + c.encode()

NAMES = ["black", "red", "green", "brown", "blue", "magenta", "cyan", "lgrey",
         "dgrey", "lred", "lgreen", "yellow", "lblue", "lmagenta", "lcyan", "white"]


class Screen:
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.ch = [[" "] * cols for _ in range(rows)]
        self.fg = [[7] * cols for _ in range(rows)]
        self.bg = [[0] * cols for _ in range(rows)]
        self.x = self.y = 0
        self.cf, self.cb = 7, 0
        self.pending = False
        self.buf = b""
        self.answers = b""
        self.bytes_in = 0

    def put(self, c):
        if self.pending:
            self.pending = False
            self.x = 0
            self.y = min(self.y + 1, self.rows - 1)
        self.ch[self.y][self.x] = c
        self.fg[self.y][self.x] = self.cf
        self.bg[self.y][self.x] = self.cb
        if self.x + 1 >= self.cols:
            self.pending = True
        else:
            self.x += 1

    def feed(self, data):
        self.bytes_in += len(data)
        self.buf += data
        s = self.buf
        i = 0
        while i < len(s):
            b = s[i]
            if b == 0x1b:
                m = re.match(rb"\x1b\[([?0-9;]*)([A-Za-z~])", s[i:])
                if not m:
                    if len(s) - i < 16:
                        break
                    i += 1
                    continue
                self.csi(m.group(1).decode(), m.group(2).decode())
                i += m.end()
                continue
            if b == 13:
                self.x = 0; self.pending = False; i += 1; continue
            if b == 10:
                self.y = min(self.y + 1, self.rows - 1); self.pending = False; i += 1; continue
            if b < 32:
                i += 1; continue
            n = 1 if b < 0x80 else 2 if b < 0xe0 else 3 if b < 0xf0 else 4
            if i + n > len(s):
                break
            self.put(s[i:i + n].decode("utf-8", "replace"))
            i += n
        self.buf = s[i:]

    def csi(self, p, f):
        ps = [int(v) if v.isdigit() else 0 for v in p.replace("?", "").split(";")] if p else []
        if f == "H":
            self.y = max(0, min(self.rows - 1, (ps[0] if ps and ps[0] else 1) - 1))
            self.x = max(0, min(self.cols - 1, (ps[1] if len(ps) > 1 and ps[1] else 1) - 1))
            self.pending = False
        elif f == "m":
            for v in ps or [0]:
                if v == 0: self.cf, self.cb = 7, 0
                elif 30 <= v <= 37: self.cf = v - 30
                elif 90 <= v <= 97: self.cf = v - 90 + 8
                elif 40 <= v <= 47: self.cb = v - 40
                elif 100 <= v <= 107: self.cb = v - 100 + 8
        elif f == "J":
            for y in range(self.rows):
                for x in range(self.cols):
                    self.ch[y][x] = " "; self.fg[y][x] = self.cf; self.bg[y][x] = self.cb
        elif f == "t" and ps and ps[0] == 18:
            self.answers += b"\x1b[8;%d;%dt" % (self.rows, self.cols)

    def text(self):
        return "\n".join("".join(r) for r in self.ch)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prog")
    ap.add_argument("-r", type=int, default=30)
    ap.add_argument("-c", type=int, default=100)
    ap.add_argument("--file")
    ap.add_argument("--attrs", type=int, action="append", default=[])
    ap.add_argument("--wait", type=float, default=0.25)
    ap.add_argument("--serial", help="a board's console: PROG is then the command line to type")
    ap.add_argument("keys", nargs="*")
    a = ap.parse_args()
    scr = Screen(a.r, a.c)
    pid = None
    if a.serial:
        import serial
        port = serial.Serial(a.serial, 115200, timeout=0)
        if a.prog != "-":          # "-" joins what is already running
            port.write(b"\r")
            time.sleep(0.4)
            port.read(65536)
            port.write(a.prog.encode() + b"\r")
        rd = lambda: port.read(65536)
        wr = port.write
        fd = port.fileno()
    else:
        pid, fd = pty.fork()
        if pid == 0:
            args = [a.prog] + ([a.file] if a.file else [])
            os.execv(a.prog, args)
        import fcntl, struct, termios
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", a.r, a.c, 0, 0))
        rd = lambda: os.read(fd, 65536)
        wr = lambda d: os.write(fd, d)

    def pump(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    d = rd()
                except OSError:
                    return False
                if not d and not a.serial:
                    return False
                scr.feed(d)
                if scr.answers:
                    wr(scr.answers); scr.answers = b""
        return True

    def show(tag):
        print("=" * 20, tag, "(%d bytes so far)" % scr.bytes_in)
        print(scr.text())
        for row in a.attrs:
            runs, last, start = [], None, 0
            for x in range(scr.cols + 1):
                cur = (scr.fg[row][x], scr.bg[row][x]) if x < scr.cols else None
                if cur != last:
                    if last is not None:
                        runs.append("%d-%d %s/%s" % (start, x - 1, NAMES[last[0]], NAMES[last[1]]))
                    last, start = cur, x
            print("row %d: %s" % (row, "; ".join(runs)))

    alive = pump(1.5 if a.serial else 0.6)
    step = 0
    for k in a.keys:
        if k == "|":
            show("step %d" % step)
            continue
        step += 1
        data = KEYS.get(k)
        if data is None:
            data = k.encode()
        wr(data)
        alive = pump(a.wait)
        if not alive:
            break
    show("end" if alive else "exited")
    if pid:
        try:
            os.kill(pid, 9)
        except OSError:
            pass


main()
