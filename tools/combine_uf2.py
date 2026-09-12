#!/usr/bin/env python3
"""Combine UF2 files into one, for shipping a whole product as a single file.

    tools/combine_uf2.py out.uf2 os_kernel.uf2 system.uf2 app.uf2

A UF2 is a sequence of 512-byte blocks and each block carries its own target
address, so one file can write several discontiguous regions -- the kernel at
0x10000000, the system's modules at 0x10100000 and an application's at
0x10800000. Only two fields have to be rewritten: blockNo, which must count from
zero across the whole file, and numBlocks, which the bootrom uses to know when
it is done and which every block repeats.

The blocks are emitted in the order the files are given, and no attempt is made
to sort or merge them. Nothing requires it: each block says where it goes.
"""
import struct
import sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
BLOCK = 512
FLAG_FAMILY_PRESENT = 0x2000

# RP2350's absolute block, which the SDK puts at the top of flash in some
# configurations -- the Fruit Jam kernel has one and the Waveshare one does not.
# It is a single block in its own family, addressed absolutely, and it is not a
# program: it must be carried through and it must not be read as a disagreement
# about which machine the file is for.
FAMILY_ABSOLUTE = 0xE48BFF57


def blocks_of(path):
    data = open(path, "rb").read()
    if len(data) % BLOCK:
        sys.exit(f"combine_uf2: {path} is {len(data)} bytes, not a whole number of blocks")
    for i in range(0, len(data), BLOCK):
        b = bytearray(data[i:i + BLOCK])
        m0, m1 = struct.unpack_from("<II", b, 0)
        (end,) = struct.unpack_from("<I", b, 508)
        if m0 != MAGIC0 or m1 != MAGIC1 or end != MAGIC_END:
            sys.exit(f"combine_uf2: {path} block {i // BLOCK} is not a UF2 block")
        yield b


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[2].strip())

    out, inputs = sys.argv[1], sys.argv[2:]
    blocks, families = [], set()
    for path in inputs:
        n = 0
        for b in blocks_of(path):
            (flags,) = struct.unpack_from("<I", b, 8)
            if flags & FLAG_FAMILY_PRESENT:
                families.add(struct.unpack_from("<I", b, 28)[0])
            blocks.append(b)
            n += 1
        print(f"  {path}: {n} blocks")

    # Two program families in one file would mean an Arm build and a RISC-V one
    # combined, and half of it would be refused by the bootrom -- or worse, half
    # accepted. The error to make is the one that says so here.
    program = families - {FAMILY_ABSOLUTE}
    if len(program) > 1:
        sys.exit("combine_uf2: these files are for different machines: " +
                 ", ".join(f"0x{f:08x}" for f in sorted(program)))
    if not program:
        sys.exit("combine_uf2: no program blocks in any of these files")

    total = len(blocks)
    with open(out, "wb") as f:
        for i, b in enumerate(blocks):
            struct.pack_into("<II", b, 20, i, total)
            f.write(b)

    # The span is of the program blocks only. The absolute block sits at the top
    # of flash and would make every range look like the whole chip.
    prog = [b for b in blocks
            if struct.unpack_from("<I", b, 28)[0] != FAMILY_ABSOLUTE]
    lo = min(struct.unpack_from("<I", b, 12)[0] for b in prog)
    hi = max(struct.unpack_from("<I", b, 12)[0] +
             struct.unpack_from("<I", b, 16)[0] for b in prog)
    extra = total - len(prog)
    print(f"  {out}: {total} blocks, 0x{lo:08x} to 0x{hi:08x}" +
          (f", and {extra} absolute" if extra else ""))


if __name__ == "__main__":
    main()
