#!/usr/bin/env python3
"""Bakar in en byggd .mod-fil i kärnbilden som en C-array.

QEMU la modulen på en fast adress med -device loader. På hårdvara finns ingen
sådan, så modulen får följa med i flash tills SD-laddningen finns. Formen är
densamma: kärnan får en pekare till ett modulhuvud och bryr sig inte om varifrån
det kom.
"""
import sys

src, dst, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()

with open(dst, "w") as f:
    f.write("// Genererad av make_blob.py. Redigera inte.\n")
    f.write("#include <stdint.h>\n\n")
    # 4-byte-justerad: modulhuvudet läses som 32-bitars ord.
    f.write(f"__attribute__((aligned(4))) const uint8_t {symbol}[] = {{\n")
    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        f.write(f"    {row},\n")
    f.write("};\n")
    f.write(f"const uint32_t {symbol}_size = {len(data)};\n")

print(f"  {dst}: {len(data)} byte som {symbol}")
