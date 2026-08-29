#!/usr/bin/env python3
import sys
import struct

# Use the intended sync word, or keep 0x0509000B for RISC-V hardware protection.
# Let's use 0x0509000B since it actively prevents CPU execution crashes via Custom-0!
MYRTOS_SYNC = 0x0509000B

def find_entry_offset(elf_path, nm_tool, symbol):
    """Where the module's entry point sits in the raw binary.

    objcopy -O binary writes from the lowest loaded address, so the offset is the
    symbol's address minus that lowest one. Assuming zero went wrong: the linker
    put myrtos_syscall first, and the kernel called it instead of module_main.
    """
    import subprocess
    out = subprocess.run([nm_tool, "-n", elf_path], capture_output=True, text=True).stdout
    addrs, entry = [], None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTdDrR":
            addrs.append(int(parts[0], 16))
            if parts[2] == symbol:
                entry = int(parts[0], 16)
    if entry is None:
        raise SystemExit(f"hittar inte symbolen {symbol} i {elf_path}")
    return entry - min(addrs)


def create_module(input_bin_path, output_mod_path, module_name,
                  elf_path=None, nm_tool=None, entry_symbol="module_main",
                  module_type="program"):
    with open(input_bin_path, "rb") as f:
        code_bytes = f.read()

# A data module -- a device descriptor, a table, a font -- has no entry
# point. exec_offset becomes zero and the kernel does not start it.
    entry_in_code = 0
    if module_type != "data" and elf_path and nm_tool:
        entry_in_code = find_entry_offset(elf_path, nm_tool, entry_symbol)

# 4-byte alignment on the code
    if len(code_bytes) % 4 != 0:
        code_bytes += b'\x00' * (4 - (len(code_bytes) % 4))

    # Eight characters, because the module directory holds names in the 8.3 form
    # a FAT card gives them. A longer name was silently cut short: the module
    # built, loaded and registered, and then could not be run because no name
    # the user could type would ever match it.
    if len(module_name) > 8:
        sys.exit(f"module name '{module_name}' is longer than 8 characters; "
                 f"the module directory stores 8.3 names and would cut it to "
                 f"'{module_name[:8]}'")

    name_bytes = module_name.encode('utf-8') + b'\x00'
    if len(name_bytes) % 4 != 0:
        name_bytes += b'\x00' * (4 - (len(name_bytes) % 4))

# The header is 28 bytes: 3 x uint32, 2 x uint16, 3 x uint32 -- the same as
# myrtos_module_header_t in common/modules.h. It said 32 here, which pushed
# exec_offset and name_offset four bytes wrong into the code.
    header_size = 28
    exec_offset = 0 if module_type == "data" else header_size + entry_in_code
    name_offset = header_size + len(code_bytes)
    module_size = header_size + len(code_bytes) + len(name_bytes)

# Defaults for the myrtos-specific fields
    MYRTOS_TYPE_PROGRAM = 1
    MYRTOS_TYPE_DATA    = 3
    kind = MYRTOS_TYPE_DATA if module_type == "data" else MYRTOS_TYPE_PROGRAM
    type_lang = (kind << 8) | 1  # high byte: type, low byte: language (C)
# High byte: attributes (re-entrant). Low byte: ABI version, which the kernel
# compares against its own and rejects on a mismatch -- otherwise a module
# built against an old interface runs until it fails somewhere obscure.
    MYRTOS_ABI_VERSION = 1
    attr_rev  = (1 << 8) | MYRTOS_ABI_VERSION
# Total RAM: data area at the bottom and the process stack from the top. One
# trap frame is 128 bytes, so 4 kB leaves ample depth for call chains.
    mem_size = 4096

# The checksum covers the first six 32-bit words of the header AS THEY LIE IN
# MEMORY, and is stored in the seventh. Two bugs lived here: the field type_lang
# precedes attr_rev in a little-endian struct, hence
# (attr_rev << 16) | type_lang and not the other way round; and the kernel summed
# seven words, which counted the crc field itself into its own sum.
    fields = [
        MYRTOS_SYNC, module_size, name_offset,
        (attr_rev << 16) | type_lang,
        exec_offset, mem_size
    ]
    header_crc = (~sum(fields)) & 0xFFFFFFFF

    # Pack the header: sync, size, name, type_lang, attr_rev, exec, mem, crc
    # Formatering: '<IIIHHIII' -> 3xUInt32, 2xUInt16, 3xUInt32
    header_bytes = struct.pack('<IIIHHIII', 
        MYRTOS_SYNC, module_size, name_offset,
        type_lang, attr_rev, exec_offset, mem_size, header_crc
    )

    with open(output_mod_path, "wb") as f:
        f.write(header_bytes)
        f.write(code_bytes)
        f.write(name_bytes)

    print(f"🎉 Myrtos-modul '{module_name}' skapad (32-bytes header matchad)!")

if __name__ == "__main__":
# --data as a flag rather than a positional argument: CMake drops empty
# positional arguments, so a data module was built as a program.
    argv = sys.argv[1:]
    module_type = "data" if "--data" in argv else "program"
    argv = [a for a in argv if a != "--data"]
    create_module(*argv, module_type=module_type)
