#!/usr/bin/env python3
import sys
import struct

# Use the intended sync word, or keep 0x0509000B for RISC-V hardware protection.
# Let's use 0x0509000B since it actively prevents CPU execution crashes via Custom-0!
MYRTOS_SYNC = 0x0509000B

def find_entry_offset(elf_path, nm_tool, symbol):
    """Where the module's entry point sits in the raw binary.

    objcopy -O binary writes from the lowest loaded address, so the offset is the
    symbol's address minus that address. Two bugs have lived here. Assuming zero
    went wrong: the linker put myrtos_syscall first and the kernel called it
    instead of module_main. Then taking the lowest symbol address went wrong once
    modules had thread-local variables: their nm addresses are offsets inside the
    TLS block, starting at zero, so the base became zero and exec_offset became
    the entry point's absolute address -- a jump into nothing.

    The base therefore comes from the program headers, which is what objcopy
    itself follows.
    """
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-lW", elf_path],
                         capture_output=True, text=True).stdout
    bases = [int(m.group(1), 16)
             for m in re.finditer(r"^\s*LOAD\s+\S+\s+0x(\S+)", out, re.M)]
    if not bases:
        raise SystemExit(f"no LOAD segment in {elf_path}")
    base = min(bases)

    out = subprocess.run([nm_tool, elf_path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == symbol and parts[1] in "tTdDrR":
            return int(parts[0], 16) - base
    raise SystemExit(f"symbol {symbol} not found in {elf_path}")


def tls_layout(elf_path, nm_tool):
    """The module's thread-local block: where its image sits, and the two sizes.

    The linker gathers .tdata and .tbss from every source file into one TLS
    segment and gives each variable a fixed offset from tp. FileSiz is the part
    with initial values that has to be copied per process; MemSiz is the whole
    block, the rest of which is zeroed. This is what OS-9's linker did with the
    data section, and what lets `static` work across the files of one module.
    """
    import subprocess, os, re
    if not elf_path or not nm_tool or not nm_tool.endswith("nm"):
        return b"", 0, 0
    prefix = nm_tool[:-2]

    out = subprocess.run([prefix + "readelf", "-lW", elf_path],
                         capture_output=True, text=True).stdout
    m = re.search(r"^\s*TLS\s+\S+\s+0x(\S+)\s+\S+\s+0x(\S+)\s+0x(\S+)", out, re.M)
    if not m:
        return 0, 0, 0
    vaddr, init, total = (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16))
    total = (total + 3) & ~3

    # The initial image is already inside what objcopy writes -- .tdata is part
    # of the loaded segment -- so the header points at that copy rather than the
    # module carrying a second one.
    bases = [int(b.group(1), 16)
             for b in re.finditer(r"^\s*LOAD\s+\S+\s+0x(\S+)", out, re.M)]
    return vaddr - min(bases), init, total


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

    tls_in_code, tls_init, tls_total = tls_layout(elf_path, nm_tool)

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

# The header is 40 bytes: ten 32-bit words, with type_lang and attr_rev sharing
# one. It must match myrtos_module_header_t exactly; when this said 28 and the
# struct said 32, exec_offset and name_offset landed four bytes into the code.
    header_size = 40
    exec_offset = 0 if module_type == "data" else header_size + entry_in_code

    # The module is header, code, then the name. The thread-local image needs no
    # room of its own: .tdata is inside the code objcopy wrote, so the header
    # just says where in it.
    tls_offset = (header_size + tls_in_code) if tls_init else 0
    name_offset = header_size + len(code_bytes)
    module_size = name_offset + len(name_bytes)

# Defaults for the myrtos-specific fields
    MYRTOS_TYPE_PROGRAM = 1
    MYRTOS_TYPE_DATA    = 3
    kind = MYRTOS_TYPE_DATA if module_type == "data" else MYRTOS_TYPE_PROGRAM
    type_lang = (kind << 8) | 1  # high byte: type, low byte: language (C)
# High byte: attributes (re-entrant). Low byte: ABI version, which the kernel
# compares against its own and rejects on a mismatch -- otherwise a module
# built against an old interface runs until it fails somewhere obscure.
    MYRTOS_ABI_VERSION = 2
    attr_rev  = (1 << 8) | MYRTOS_ABI_VERSION
# Total RAM: data area at the bottom and the process stack from the top. One
# trap frame is 128 bytes, so 4 kB leaves ample depth for call chains.
    mem_size = 4096

# The checksum covers the first nine 32-bit words AS THEY LIE IN MEMORY, and is
# stored in the tenth. Two bugs lived here: type_lang precedes attr_rev in a
# little-endian struct, hence (attr_rev << 16) | type_lang and not the other way
# round; and the kernel once summed the crc field into its own checksum.
    fields = [
        MYRTOS_SYNC, module_size, name_offset,
        (attr_rev << 16) | type_lang,
        exec_offset, mem_size,
        tls_offset, tls_init, tls_total
    ]
    header_crc = (~sum(fields)) & 0xFFFFFFFF

    header_bytes = struct.pack('<IIIHHIIIIII',
        MYRTOS_SYNC, module_size, name_offset,
        type_lang, attr_rev, exec_offset, mem_size,
        tls_offset, tls_init, tls_total, header_crc
    )
    assert len(header_bytes) == header_size, "header is not %d bytes" % header_size

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
