#!/usr/bin/env python3
"""Kontrollerar att en modul verkligen är positionsoberoende och delbar.

Två egenskaper avgör om en modul kan laddas på en okänd adress och delas
mellan processer, och ingen av dem syns i källkoden:

  1. Inga absoluta adresser i allokerade sektioner. PC-relativa hopp och
     strängar följer med när modulen flyttas; en funktionspekare i en tabell
     gör det inte -- den är en absolut adress som länkaren skrev in. I C är det
     en `static const struct { void (*fn)(void); }`, i Rust varje `dyn Trait`.

  2. Inga skrivbara sektioner. Finns .data eller .bss i modulbilden skriver två
     processer som delar koden i samma variabler.

Relokeringar i felsökningssektionerna räknas inte: de följer aldrig med när
objcopy plockar ut den råa binären.
"""
import subprocess, sys, re

readelf, obj_files, elf = sys.argv[1], sys.argv[2:-1], sys.argv[-1]

# PC-relativa och rent lokala typer. Allt annat i en allokerad sektion är en
# absolut adress.
POSITION_INDEPENDENT = {
    "R_RISCV_PCREL_HI20", "R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
    "R_RISCV_BRANCH", "R_RISCV_JAL", "R_RISCV_RVC_BRANCH", "R_RISCV_RVC_JUMP",
    "R_RISCV_CALL", "R_RISCV_CALL_PLT", "R_RISCV_RELAX", "R_RISCV_ALIGN",
    "R_RISCV_ADD8", "R_RISCV_ADD16", "R_RISCV_ADD32", "R_RISCV_ADD64",
    "R_RISCV_SUB6", "R_RISCV_SUB8", "R_RISCV_SUB16", "R_RISCV_SUB32",
    "R_RISCV_SUB64", "R_RISCV_SET6", "R_RISCV_SET8", "R_RISCV_SET16",
    "R_RISCV_SET32", "R_RISCV_SET_ULEB128", "R_RISCV_SUB_ULEB128",
}

problems = []

for obj in obj_files:
    out = subprocess.run([readelf, "-W", "-r", obj], capture_output=True, text=True).stdout
    section = None
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            section = m.group(1)
            continue
        m = re.search(r"(R_RISCV_\w+)", line)
        if not m or not section:
            continue
        # Felsökningsinformation laddas aldrig.
        if section.startswith(".rela.debug") or section.startswith(".rela.eh_frame"):
            continue
        kind = m.group(1)
        if kind not in POSITION_INDEPENDENT:
            # C++ lägger vtabellen i en egen sektion vars namn bär det manglade
            # klassnamnet. Avmanglat blir felet begripligt utan ABI-kunskap.
            hint = section
            m2 = re.search(r"(_Z\S+)", section)
            if m2:
                dem = subprocess.run(["c++filt", m2.group(1)],
                                     capture_output=True, text=True).stdout.strip()
                if dem and dem != m2.group(1):
                    hint = f"{section}  ({dem})"
            problems.append(f"{obj}: {kind} i {hint}")

out = subprocess.run([readelf, "-W", "-S", elf], capture_output=True, text=True).stdout
for line in out.splitlines():
    m = re.search(r"\]\s+(\.\S+)\s+\S+\s+\S+\s+\S+\s+(\S+)", line)
    if m and m.group(1) in (".data", ".bss", ".sdata", ".sbss"):
        if m.group(2) != "000000":
            problems.append(f"{elf}: skrivbar sektion {m.group(1)} finns")

if problems:
    print("MODULEN ÄR INTE POSITIONSOBEROENDE:", file=sys.stderr)
    seen = set()
    for p in problems:
        if p in seen: continue
        seen.add(p)
        print(f"  {p}", file=sys.stderr)
    print("\nEn absolut adress i en allokerad sektion betyder oftast en statisk",
          file=sys.stderr)
    print("funktionspekare, en vtable, eller en pekare till en statisk variabel.",
          file=sys.stderr)
    sys.exit(1)

print(f"  {elf}: positionsoberoende och delbar")
