#!/usr/bin/env python3
"""Concatenate .mod files into an image written to the module region in flash.

The modules are laid out one after another, four-byte aligned. The kernel finds
them by searching for the sync word -- no directory, no filesystem. That is what
OS-9 did with ROM: the module IS its own directory entry.
"""
import sys

out = sys.argv[1]
mods = sys.argv[2:]

blob = bytearray()
for m in mods:
    data = open(m, "rb").read()
    blob += data
    while len(blob) % 4:
        blob.append(0)

open(out, "wb").write(blob)
print(f"  {out}: {len(mods)} moduler, {len(blob)} byte")
