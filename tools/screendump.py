#!/usr/bin/env python3
"""Print what is on the character-generator screen, read over the Debug Probe.

    tools/screendump.py [build-dir]

For a board whose display cannot be photographed into a terminal -- and for
checking a layout without asking anyone to squint at the panel. The chargen
keeps a ring of cells in SRAM; a cell's ch is the glyph index, which is the
character minus 32, so the inverse is exact. row_origin says which ring slot is
the top of the live screen, and the scrollback lives in the same ring behind it.

Both symbols are static, which is no obstacle: nm lists them anyway, and reading
them out of the ELF is what keeps this working when the kernel is relinked.
"""
import os, subprocess, sys

OCD = os.path.expanduser("~/.pico-sdk/openocd/0.12.0+dev")
TOOLCHAIN = os.path.expanduser("~/.pico-sdk/toolchain/15_2_Rel1/bin")
BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
COLS, ROWS, RING = 100, 30, 256          # as MYRTOS_CELL_* say for this panel


def symbol(name):
    out = subprocess.run([TOOLCHAIN + "/arm-none-eabi-nm", BUILD + "/os_kernel.elf"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name:
            return int(f[0], 16)
    sys.exit("screendump: no symbol " + name + " in " + BUILD + "/os_kernel.elf")


def openocd(*commands):
    argv = [OCD + "/openocd", "-s", OCD + "/scripts",
            "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
            "-c", "adapter speed 5000", "-c", "init"]
    for c in commands:
        argv += ["-c", c]
    r = subprocess.run(argv + ["-c", "exit"], capture_output=True, text=True)
    return r.stdout + r.stderr


cells, origin = symbol("cells"), symbol("row_origin")
dump = BUILD + "/cells.bin"

# One session for both, so the origin cannot move between the two reads and
# leave the picture sheared.
out = openocd(f"dump_image {dump} 0x{cells:08x} 0x{RING * COLS * 2:x}",
              f"mdw 0x{origin:08x} 1")
top = 0
for line in out.splitlines():
    if line.startswith("0x") and ":" in line:
        top = int(line.split(":")[1].split()[0], 16)

data = open(dump, "rb").read()
print("+" + "-" * COLS + "+")
for row in range(ROWS):
    base = ((top + row) % RING) * COLS * 2
    text = "".join(" " if data[base + c * 2] == 0 else chr(data[base + c * 2] + 32)
                   for c in range(COLS))
    print("|" + text.rstrip().ljust(COLS) + "|")
print("+" + "-" * COLS + "+")
