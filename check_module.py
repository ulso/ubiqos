#!/usr/bin/env python3
"""Check that a module really is position independent and shareable.

Two properties decide whether a module can be loaded at an unknown address and
shared between processes, and neither shows in the source:

  1. No absolute addresses in allocated sections. PC-relative jumps and strings
     travel with the module when it moves; a function pointer in a table does
     not -- it is an absolute address the linker wrote in. In C that is a
     `static const struct { void (*fn)(void); }`, in Rust every `dyn Trait`.

  2. No writable sections. If .data or .bss are present in the module image,
     two processes sharing the code write to the same variables.

Relocations in the debug sections do not count: they never travel along when
objcopy extracts the raw binary.
"""
import subprocess, sys, re

readelf, obj_files, elf = sys.argv[1], sys.argv[2:-1], sys.argv[-1]

# PC-relative and purely local types. Anything else in an allocated section is a
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
# Debug information is never loaded.
        if section.startswith(".rela.debug") or section.startswith(".rela.eh_frame"):
            continue
        kind = m.group(1)
        if kind not in POSITION_INDEPENDENT:
# C++ puts the vtable in a section of its own whose name carries the mangled
# class name. Demangled, the error is understandable without ABI knowledge.
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
    print("MODULE IS NOT POSITION INDEPENDENT:", file=sys.stderr)
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
