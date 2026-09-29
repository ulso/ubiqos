#!/usr/bin/env python3
"""Concatenate .mod files into an image written to the module region in flash.

The modules are laid out one after another, four-byte aligned. The kernel finds
them by searching for the sync word -- no directory, no filesystem. That is what
OS-9 did with ROM: the module IS its own directory entry.

    make_flash_image.py [--base ADDRESS [--data-base ADDRESS --data-size BYTES]]
                        OUT.bin MODULE.mod ...

With --base, the address the image is written to, the modules are also PLACED:
each starts on a sixteen-byte boundary, and one marked UBIQOS_ATTR_PLACEABLE --
private only because it has addresses to fix -- has them fixed here, for the
address it will lie at, and loses its PRIVATE bit. The kernel then runs it in
place, where it would otherwise have copied the whole module into RAM to apply
two relocations. See UBIQOS_ATTR_PLACEABLE in common/ubiqos_abi.h.

With --data-base as well, a module marked UBIQOS_ATTR_SPLIT -- one instance,
its writable data one tail after its code -- is placed too: its code and
constants for where they lie, its data for a fixed address in the board's data
area, handed out here in turn until the area is full. The kernel copies only
that tail when the program starts. See UBIQOS_ATTR_SPLIT.

Sixteen, because that is what the kernel aligns a copy to and what a module's
sections were checked against when it was built. The padding is counted into
the module's own size, so the kernel's walk -- which steps from one module to
the next by that size -- lands on the next one and not on the padding.
"""
import struct
import sys

HEADER = "<IIIHHIIIIIHHIIII"          # common/ubiqos_abi.h, ubiqos_module_header_t
HEADER_SIZE = struct.calcsize(HEADER)
ATTR_PRIVATE = 0x04
ATTR_PLACEABLE = 0x20
ATTR_SPLIT = 0x40
SPLIT_MAGIC = 0x54494c53

RELOC_ABS32, RELOC_HI20, RELOC_LO12_I, RELOC_LO12_S = 0, 1, 2, 3


def get32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def put32(b, at, v):
    struct.pack_into("<I", b, at, v & 0xFFFFFFFF)


def relocate(image, header, base, split=None):
    """Fix every relocation for a module whose header lies at BASE: the same
    arithmetic as relocate_image in kernel/scheduler.c, which does it for a copy.
    With SPLIT = (offset, address), a target at or past OFFSET is in the
    writable tail and is put at ADDRESS instead."""
    reloc_offset, reloc_count = header[12], header[13]
    for i in range(reloc_count):
        w0 = get32(image, reloc_offset + i * 8)
        target = get32(image, reloc_offset + i * 8 + 4)
        if split and target >= split[0]:
            v = (split[1] + target - split[0]) & 0xFFFFFFFF
        else:
            v = (base + target) & 0xFFFFFFFF
        site = w0 & 0x0FFFFFFF
        kind = w0 >> 28
        if kind == RELOC_ABS32:
            put32(image, site, v)
        elif kind == RELOC_HI20:
            put32(image, site, (get32(image, site) & 0x00000FFF) | ((v + 0x800) & 0xFFFFF000))
        elif kind == RELOC_LO12_I:
            put32(image, site, (get32(image, site) & 0x000FFFFF) | ((v & 0xFFF) << 20))
        elif kind == RELOC_LO12_S:
            lo = v & 0xFFF
            put32(image, site, (get32(image, site) & ~0xFE000F80 & 0xFFFFFFFF)
                  | ((lo >> 5) << 25) | ((lo & 0x1F) << 7))


def rewrite_header(image, header):
    """The header with its checksum recomputed: the complement of the sum of
    the first thirteen words, which is what verify_ubiqos_header checks."""
    struct.pack_into(HEADER, image, 0, *header)
    words = struct.unpack_from("<13I", image, 0)
    crc = (~sum(words)) & 0xFFFFFFFF
    header = list(header)
    header[15] = crc
    struct.pack_into(HEADER, image, 0, *header)


class DataArea:
    """The board's fixed data area, handed out a module at a time."""
    def __init__(self, base, size):
        self.next, self.end = base, base + size

    def take(self, n):
        n = (n + 15) & ~15
        if self.next + n > self.end:
            return None
        at, self.next = self.next, self.next + n
        return at


def place(data, base, area=None):
    """One module laid out for BASE: padded to sixteen, and relocated there if
    it can run in place. Returns the bytes and how it was placed, if it was."""
    image = bytearray(data)
    while len(image) % 16:
        image.append(0)
    header = list(struct.unpack_from(HEADER, image, 0))
    header[1] = len(image)                       # module_size, padding included
    attrs = header[4] >> 8
    placed = None
    if attrs & ATTR_PLACEABLE and attrs & ATTR_PRIVATE:
        relocate(image, header, base)
        header[4] &= ~(ATTR_PRIVATE << 8)
        placed = "in place"
    elif attrs & ATTR_SPLIT and attrs & ATTR_PRIVATE and area:
        trailer = header[12] + 8 * header[13]
        magic, data_offset = struct.unpack_from("<II", image, trailer)
        if magic == SPLIT_MAGIC:
            tail = header[2] - data_offset + header[14]      # image tail and .bss
            at = area.take(tail)
            if at is not None:
                relocate(image, header, base, (data_offset, at))
                struct.pack_into("<I", image, trailer + 8, at)
                header[4] &= ~(ATTR_PRIVATE << 8)
                placed = f"code in place, {tail} bytes of data at 0x{at:08x}"
    rewrite_header(image, header)
    return bytes(image), placed


def main():
    args = sys.argv[1:]
    opts = {}
    while args and args[0] in ("--base", "--data-base", "--data-size"):
        opts[args[0]] = int(args[1], 0)
        args = args[2:]
    base = opts.get("--base")
    if base is not None and base % 16:
        sys.exit("make_flash_image.py: --base must be a multiple of 16")
    area = None
    if "--data-base" in opts:
        if base is None or "--data-size" not in opts:
            sys.exit("make_flash_image.py: --data-base needs --base and --data-size")
        area = DataArea(opts["--data-base"], opts["--data-size"])
    out, mods = args[0], args[1:]

    blob = bytearray()
    placed = 0
    for m in mods:
        data = open(m, "rb").read()
        if base is not None:
            data, how = place(data, base + len(blob), area)
            if how:
                placed += 1
                if how != "in place":
                    name_offset = struct.unpack_from("<I", data, 8)[0]
                    name = data[name_offset:data.index(b"\0", name_offset)].decode()
                    print(f"  {name}: {how}")
        blob += data
        while len(blob) % 4:
            blob.append(0)

    # A terminator, so the kernel knows where the image ends. Without it a smaller
    # image loaded over a larger one leaves the old tail behind, and the scan reads
    # those modules too -- picotool writes only as many bytes as the file has.
    blob += b"\x00\x00\x00\x00"

    open(out, "wb").write(blob)
    where = "" if base is None else f", {placed} of them placed to run at 0x{base:08x}"
    print(f"  {out}: {len(mods)} modules, {len(blob)} bytes{where}")


if __name__ == "__main__":
    main()
