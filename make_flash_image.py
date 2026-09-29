#!/usr/bin/env python3
"""Concatenate .mod files into an image written to the module region in flash.

The modules are laid out one after another, four-byte aligned. The kernel finds
them by searching for the sync word -- no directory, no filesystem. That is what
OS-9 did with ROM: the module IS its own directory entry.

    make_flash_image.py [--base ADDRESS] OUT.bin MODULE.mod ...

With --base, the address the image is written to, the modules are also PLACED:
each starts on a sixteen-byte boundary, and one marked UBIQOS_ATTR_PLACEABLE --
private only because it has addresses to fix -- has them fixed here, for the
address it will lie at, and loses its PRIVATE bit. The kernel then runs it in
place, where it would otherwise have copied the whole module into RAM to apply
two relocations. See UBIQOS_ATTR_PLACEABLE in common/ubiqos_abi.h.

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

RELOC_ABS32, RELOC_HI20, RELOC_LO12_I, RELOC_LO12_S = 0, 1, 2, 3


def get32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def put32(b, at, v):
    struct.pack_into("<I", b, at, v & 0xFFFFFFFF)


def relocate(image, header, base):
    """Fix every relocation for a module whose header lies at BASE: the same
    arithmetic as relocate_image in kernel/scheduler.c, which does it for a copy."""
    reloc_offset, reloc_count = header[12], header[13]
    for i in range(reloc_count):
        w0 = get32(image, reloc_offset + i * 8)
        v = (base + get32(image, reloc_offset + i * 8 + 4)) & 0xFFFFFFFF
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


def place(data, base):
    """One module laid out for BASE: padded to sixteen, and relocated there if
    it can run in place."""
    image = bytearray(data)
    while len(image) % 16:
        image.append(0)
    header = list(struct.unpack_from(HEADER, image, 0))
    header[1] = len(image)                       # module_size, padding included
    attrs = header[4] >> 8
    placed = False
    if attrs & ATTR_PLACEABLE and attrs & ATTR_PRIVATE:
        relocate(image, header, base)
        header[4] &= ~(ATTR_PRIVATE << 8)
        placed = True
    rewrite_header(image, header)
    return bytes(image), placed


def main():
    args = sys.argv[1:]
    base = None
    if len(args) >= 2 and args[0] == "--base":
        base = int(args[1], 0)
        args = args[2:]
        if base % 16:
            sys.exit("make_flash_image.py: --base must be a multiple of 16")
    out, mods = args[0], args[1:]

    blob = bytearray()
    placed = 0
    for m in mods:
        data = open(m, "rb").read()
        if base is not None:
            data, was_placed = place(data, base + len(blob))
            placed += was_placed
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
