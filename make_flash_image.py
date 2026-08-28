#!/usr/bin/env python3
"""Slår ihop .mod-filer till en avbild som skrivs till modulregionen i flash.

Modulerna läggs efter varandra, fyrbytejusterade. Kärnan hittar dem genom att
söka efter synkordet -- ingen katalog, inget filsystem. Det är samma sak som
OS-9 gjorde med ROM: modulen ÄR sin egen katalogpost.
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
