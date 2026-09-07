#!/usr/bin/env python3
"""Watch the USB host over a long run, and say whether anything moved.

The hub failure this board has had since August is intermittent, and every
conclusion about it so far has rested on somebody watching for an evening. That
is why this exists: it works the machine and reads `usbstat` after each round,
so a claim about the bus can be a table instead of a recollection.

The work is chosen for one reason. Starting a wasm module used to kill the bus
every time -- the loader copied a third of a megabyte into PSRAM inside a trap,
with interrupts off, and three missed polls end a transfer -- so it is the
workload with a known history of doing damage. See the hub notes.

Usage:  python3 tools/usbsoak.py [rounds] [port]

Reads nothing with a probe and changes nothing on the board: the failure has
been caused by watching it before, and halting the core to look at memory is
exactly how. This asks the machine over its own console instead.
"""
import re
import sys
import threading
import time

import serial

PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbmodem0000011"
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 12

# No pager here. `more` waits for a key, so the round after it read as missing
# data the first time this was run -- a hole in the instrument that looks like a
# hole in the measurement.
WORK = ["wasm /sd/tiny.wasm", "ls /sd", "lsmod", "free"]

port = serial.Serial(PORT, 115200, timeout=0)
buf = bytearray()
stop = False


def _reader():
    # Drained continuously and not in bursts. A reader that sleeps between
    # reads loses whole replies, because the shell redraws its input line on
    # every keystroke and the port's buffer is not large.
    while not stop:
        n = port.in_waiting
        if n:
            buf.extend(port.read(n))
        else:
            time.sleep(0.005)


def run(cmd, cap=10.0):
    """Type one command and return what the terminal would have shown."""
    del buf[:]
    port.write((cmd + "\r").encode())
    port.flush()
    last, deadline = -1, time.time() + cap
    while time.time() < deadline:
        time.sleep(0.2)
        if len(buf) == last and len(buf) > 0:
            break
        last = len(buf)
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", bytes(buf).decode("utf8", "replace"))
    lines = []
    for line in text.split("\n"):
        cur = ""
        for part in line.split("\r"):        # apply the carriage returns
            cur = part + cur[len(part):]
        lines.append(cur)
    return "\n".join(lines)


def endpoint(text, device, addr):
    m = re.search(rf"device {device} (?:\(hub\) )?ep 0x0*{addr}, (\S+ ?\S*), failed (\d+)", text)
    return f"{m.group(1)},f{m.group(2)}" if m else "?"


def counter(text, label):
    m = re.search(rf"{label}:\s+(\d+)", text)
    return m.group(1) if m else "?"


threading.Thread(target=_reader, daemon=True).start()
time.sleep(0.5)

print("round  hub 0x81       kbd 0x81     kbd 0x82     rearms rec cdc keys")
worst = 0
for r in range(ROUNDS):
    run(WORK[r % len(WORK)], cap=15.0)
    stat = run("usbstat")
    hub = endpoint(stat, 6, "81")
    print(f"{r:5d}  {hub:14s} {endpoint(stat, 1, '81'):12s} {endpoint(stat, 1, '82'):12s} "
          f"{counter(stat, 'rearms'):>6s} {counter(stat, 'recoveries'):>3s} "
          f"{counter(stat, 'cdc re-arms'):>3s} {counter(stat, 'keys pushed'):>4s}",
          flush=True)
    if "NOTHING" in hub or hub.endswith(("f1", "f2", "f3")):
        worst += 1

stop = True
time.sleep(0.1)
port.close()
print(f"\n{ROUNDS - worst} of {ROUNDS} rounds healthy")
sys.exit(1 if worst else 0)
