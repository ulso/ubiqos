#!/usr/bin/env python3
import os
import re
import sys
import struct

# Use the intended sync word, or keep 0x0509000B for RISC-V hardware protection.
# Let's use 0x0509000B since it actively prevents CPU execution crashes via Custom-0!
UBIQOS_SYNC = 0x0509000B

def elf_load_base(elf_path, nm_tool):
    """The lowest loaded address, which is where objcopy starts writing."""
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-lW", elf_path],
                         capture_output=True, text=True).stdout
    bases = [int(m.group(1), 16)
             for m in re.finditer(r"^\s*LOAD\s+\S+\s+0x(\S+)", out, re.M)]
    if not bases:
        raise SystemExit(f"no LOAD segment in {elf_path}")
    return min(bases)


def find_mem_size(elf_path, nm_tool, default):
    """How much memory the module asks for, if it says.

    UBIQOS_MEM_SIZE(n) puts an ABSOLUTE symbol in the object -- no data, no
    relocation, nothing in the image -- so its nm value IS the number. A module
    that says nothing gets the default, which is what every module got before
    this existed.
    """
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([nm_tool, elf_path], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"nm failed on {elf_path}")
    m = re.search(r"^([0-9a-fA-F]+)\s+[aA]\s+__ubiqos_mem_size$", out.stdout, re.M)
    if not m:
        return default

    n = int(m.group(1), 16)
    # A stack lives at the top of this and a trap frame is 128 bytes, so a
    # thousand bytes is not a process. The ceiling is the SRAM pool's own size:
    # asking for more cannot be satisfied and should fail here, where it can be
    # read, rather than at exec where it is a number nobody sees.
    if n % 4 or not (1024 <= n <= 64 * 1024):
        raise SystemExit(f"{elf_path}: UBIQOS_MEM_SIZE({n}) is not a multiple of "
                         f"four between 1024 and 65536")
    return n


def find_entry_offset(elf_path, nm_tool, symbol):
    """Where the module's entry point sits in the raw binary.

    objcopy -O binary writes from the lowest loaded address, so the offset is the
    symbol's address minus that address. Two bugs have lived here. Assuming zero
    went wrong: the linker put ubiqos_syscall first and the kernel called it
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
        # W and V are weak definitions, which are still definitions. A module
        # built NEWLIB gets its module_main from common/ubiqos_syscalls.c as a
        # weak symbol, so that a program written for UbiqOS can define its own
        # and a ported one can just have a main.
        if len(parts) == 3 and parts[2] == symbol and parts[1] in "tTdDrRWV":
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


# A constant out of common/ubiqos_abi.h, so that this script and the kernel
# cannot disagree about it. The header is beside this file.
def abi_constant(name):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "common", "ubiqos_abi.h")
    with open(path) as f:
        m = re.search(r"^#define\s+%s\s+(\d+)" % re.escape(name), f.read(), re.M)
    if not m:
        sys.exit(f"{name} not found in {path}")
    return int(m.group(1))


def elf_machine(elf_path, nm_tool):
    """Which machine the object file is for, taken from the ELF header.

    Derived rather than declared: a build that is told its architecture can be
    told the wrong one, and the whole reason this field exists is that nothing
    downstream would notice.
    """
    import subprocess
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-h", elf_path],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "Machine:" not in line:
            continue
        what = line.split(":", 1)[1].strip()
        if "RISC-V" in what:
            return 1                      # UBIQOS_ARCH_RV32
        if what.startswith("ARM"):
            return 2                      # UBIQOS_ARCH_ARM32
        raise SystemExit("unknown machine in %s: %s" % (elf_path, what))
    return 0                              # UBIQOS_ARCH_NONE


def max_alignment(elf_path, nm_tool):
    """The strictest alignment any loaded section asks for."""
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-SW", elf_path],
                         capture_output=True, text=True).stdout
    want = 1
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3 or not line.lstrip().startswith("["):
            continue
        if parts[2] not in ("PROGBITS", "NOBITS"):
            continue
        if "A" not in line[line.rfind(parts[2]):]:
            continue
        try:
            want = max(want, int(parts[-1]))
        except ValueError:
            pass
    return want


def has_writable_data(elf_path, nm_tool):
    """Whether the module carries a writable section of its own.

    .tdata and .tbss do not count and are the point of the exception: they are
    one copy per process already, which is what tp is for. Anything else that
    is writable belongs to the module image, and a module that has one cannot
    run out of flash where every process would share it.
    """
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-SW", elf_path],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+[0-9a-fA-F]+\s+"
                     r"[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S*)", line)
        if not m:
            continue
        name, kind, size, flags = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        if name in (".tdata", ".tbss"):
            continue
        # Not code. .text comes out WAX because a module is linked -N, which
        # makes one loadable segment by design rather than a writable variable.
        # A data section is WA without the X, and check_module.py has said so in
        # a comment since before this existed.
        if "A" in flags and "W" in flags and "X" not in flags and size \
                and kind in ("PROGBITS", "NOBITS"):
            return True
    return False


def bss_after_image(elf_path, nm_tool, load_base, image_len):
    """How far the allocated sections reach past what objcopy wrote.

    .bss and .sbss are NOBITS: they have an address and a size and no bytes in
    the file. A loader that copies the image has to add this much and zero it,
    and a pointer into it is only inside the module if this is counted.
    """
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""
    out = subprocess.run([prefix + "readelf", "-SW", elf_path],
                         capture_output=True, text=True).stdout

    end = load_base + image_len
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
                     r"[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S*)", line)
        if not m:
            continue
        flags = m.group(5)
        if "A" not in flags:
            continue
        # .tdata and .tbss are the thread-local block, which the kernel places
        # and zeroes from tls_offset and tls_total. Counting them here would
        # make the copy larger than the module and, worse, would say a module
        # has writable data of its own when the whole point of tp is that it
        # does not.
        if m.group(1) in (".tdata", ".tbss"):
            continue
        addr, size = int(m.group(3), 16), int(m.group(4), 16)
        if addr and addr + size > end:
            end = addr + size
    return end - (load_base + image_len)


def collect_relocs(elf_path, nm_tool, load_base, image_len, header_size, bss_size,
                   image):
    """Every absolute address in the loadable image, with what to do about it.

    Four kinds, and they are not interchangeable. R_RISCV_32 is a 32-bit word in
    data holding an address. The other three are instructions: a module's own
    code is PC-relative under -mcmodel=medany, but newlib and libgcc arrive
    prebuilt in the toolchain's default code model and address globals
    absolutely with lui -- 282 of them in wasm, and the first one reached killed
    the board. HI20 carries the top twenty bits of a target and LO12_I/LO12_S
    the bottom twelve; none can be repaired from the instruction alone, since
    twelve bits do not carry a target back. So the table records the target and
    the loader writes the whole field.

    Which sections are loaded is read from the file rather than inferred from
    addresses: a debug section has address zero, so its offsets start near zero
    and run to its own length -- tens of kilobytes -- which overlaps the address
    range of the image and looks exactly like a relocation inside it.
    """
    import subprocess, re
    prefix = nm_tool[:-2] if nm_tool.endswith("nm") else ""

    sec = subprocess.run([prefix + "readelf", "-SW", elf_path],
                         capture_output=True, text=True).stdout
    allocated = set()
    for line in sec.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-fA-F]+\s+"
                     r"[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\S+\s+(\S*)", line)
        if m and "A" in m.group(2):
            allocated.add(m.group(1))

    # Kind 0 is a whole address sitting in a word, and both machines have it:
    # RISC-V puts one in a pointer, ARM puts one in a literal pool. Kinds 1 to 3
    # are RISC-V's instruction pairs and have no ARM counterpart, because
    # Thumb-2 does not build addresses out of immediates.
    KIND = {"R_RISCV_32": 0, "R_ARM_ABS32": 0,
            "R_RISCV_HI20": 1, "R_RISCV_LO12_I": 2, "R_RISCV_LO12_S": 3}

    out = subprocess.run([prefix + "readelf", "-rW", elf_path],
                         capture_output=True, text=True).stdout

    found = []
    section = None
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            name = m.group(1)
            target = name[5:] if name.startswith(".rela") else name[4:]
            section = target if target in allocated else None
            continue
        if not section:
            continue

        # Offset  Info  Type  [Sym.Value  Sym.Name + Addend]
        #
        # The tail is there for RELA, which RISC-V emits, and absent for REL,
        # which ARM emits and which keeps the addend in the word instead. So the
        # tail is optional here, and where the target comes from depends on the
        # kind rather than on the format: a whole address in a word can simply
        # be read back, and only the instruction pairs need the symbol.
        #
        # Getting this wrong was silent. color came out with twenty-five
        # R_ARM_ABS32 in the linked file and an empty table in the module.
        # The symbol value is there in both formats; only the "+ addend" is
        # RELA's. Requiring the plus made every ARM line fail to match at all,
        # which is how twenty-five relocations became an empty table.
        m = re.match(r"\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(\S+)"
                     r"(?:\s+([0-9a-fA-F]+)\s+\S+"
                     r"(?:\s*\+\s*([0-9a-fA-F]+))?)?\s*$", line)
        if not m or m.group(2) not in KIND:
            continue

        site = int(m.group(1), 16)
        if KIND[m.group(2)] == 0:
            at = site - load_base
            if not (0 <= at and at + 4 <= image_len):
                continue
            value = int.from_bytes(image[at:at + 4], "little")
        elif m.group(3) is None or m.group(4) is None:
            raise SystemExit("%s at 0x%08x has no symbol and addend to relocate "
                             "against" % (m.group(2), site))
        else:
            value = int(m.group(3), 16) + int(m.group(4), 16)
        if not (load_base <= site < load_base + image_len):
            continue
        # A null pointer is null wherever the module lands, so it is left alone
        # rather than relocated or refused. This is not a corner case: newlib
        # declares its optional hooks weak and undefined -- __call_exitprocs,
        # software_init_hook, _printf_float -- so that the code can test the
        # address and skip the call. Relocating those zeroes would turn every
        # "is this present?" into yes and call into the middle of the module.
        if value == 0:
            continue
        # One past the end is inside. _end and __bss_end__ name the address
        # after the last byte, which is where a heap starts and exactly what a
        # pointer to it should hold; wasm has one and a strict < refused it.
        if not (load_base <= value <= load_base + image_len + bss_size):
            raise SystemExit(
                "%s at 0x%08x points at 0x%08x, outside the module -- the "
                "loader could not relocate it" % (m.group(2), site, value))

        found.append((header_size + site - load_base,
                      KIND[m.group(2)],
                      header_size + value - load_base))
    return sorted(set(found))


def create_module(input_bin_path, output_mod_path, module_name,
                  elf_path=None, nm_tool=None, entry_symbol="module_main",
                  revision=1, realtime=False, single=False,
                  module_type="program", autostart=False, plain=False):
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

    # Fifteen characters, which is UBIQOS_NAME_LEN minus the terminator.
    # It was eight until the module directory stopped storing 8.3 names, and a
    # longer one was silently cut short: the module built, loaded and
    # registered, and then could not be run, because no name a user could type
    # would ever match it.
    NAME_MAX = 15
    if len(module_name) > NAME_MAX:
        sys.exit(f"module name '{module_name}' is longer than {NAME_MAX} characters; "
                 f"the module directory would cut it to '{module_name[:NAME_MAX]}'")

    # A module name is typed at a shell prompt and used as a filename, so it may
    # hold what both allow and nothing else. The space is refused for the first
    # reason -- one word at a prompt -- and the rest for the second. It is also
    # the space that used to make "sh" and "sh      " the same string, which was
    # a source of bugs for as long as the padding existed.
    bad = set(' \t/\\:*?"<>|') & set(module_name)
    if bad or not module_name:
        sys.exit(f"module name '{module_name}' is not usable: a name must be "
                 f"non-empty and free of {' '.join(sorted(repr(c) for c in bad)) or 'nothing'}"
                 f" -- it is typed at a prompt and used as a filename")

    name_bytes = module_name.encode('utf-8') + b'\x00'
    if len(name_bytes) % 4 != 0:
        name_bytes += b'\x00' * (4 - (len(name_bytes) % 4))

# The header is 56 bytes: fourteen 32-bit words, with type_lang and attr_rev
# sharing one and revision and reserved sharing another. It must match
# ubiqos_module_header_t exactly; when this said 28 and the struct said 32,
# exec_offset and name_offset landed four bytes into the code.
    header_size = 56
    exec_offset = 0 if module_type == "data" else header_size + entry_in_code

    # The module is header, code, then the name. The thread-local image needs no
    # room of its own: .tdata is inside the code objcopy wrote, so the header
    # just says where in it.
    tls_offset = (header_size + tls_in_code) if tls_init else 0
    name_offset = header_size + len(code_bytes)

    # Absolute pointers become offsets from the start of the module, and the
    # loader turns them back into addresses by adding where it put it. Storing
    # the offset rather than the linked address is what makes a module that was
    # never relocated fail loudly -- the pointer is a small number and faults at
    # once -- instead of pointing somewhere plausible and wrong.
    bss_size = 0
    if elf_path and nm_tool:
        bss_size = bss_after_image(elf_path, nm_tool,
                                   elf_load_base(elf_path, nm_tool), len(code_bytes))
        # The loader aligns a copied module to sixteen. A section wanting more
        # than that would land wrong wherever the allocator put it, and the
        # symptom is a misaligned access somewhere with nothing to do with the
        # cause -- so it is refused here, where the number is known.
        want = max_alignment(elf_path, nm_tool)
        if want > 16:
            raise SystemExit(
                "%s: a section needs %d-byte alignment and the loader gives 16"
                % (module_name, want))

    # Every module, single-instance included. A pointer into .bss is inside the
    # module as long as bss_size is counted -- lwipd has one, and it is what
    # made relocating these look impossible before the size was measured.
    reloc = []
    if elf_path and nm_tool:
        reloc = collect_relocs(elf_path, nm_tool,
                               elf_load_base(elf_path, nm_tool),
                               len(code_bytes), header_size, bss_size, code_bytes)

    reloc_offset = name_offset + len(name_bytes)
    reloc_count = len(reloc)
    module_size = reloc_offset + 8 * reloc_count

# Defaults for the ubiqos-specific fields
    UBIQOS_TYPE_PROGRAM = 1
    UBIQOS_TYPE_DRIVER  = 2
    UBIQOS_TYPE_DATA    = 3
    UBIQOS_TYPE_LIBRARY = 4
    kind = {"data": UBIQOS_TYPE_DATA,
            "driver": UBIQOS_TYPE_DRIVER,
            "library": UBIQOS_TYPE_LIBRARY}.get(module_type, UBIQOS_TYPE_PROGRAM)
    # High byte: type. Low byte: the machine in the high nibble, the language in
    # the low one. Both fit in four bits and always have.
    arch = elf_machine(elf_path, nm_tool) if (elf_path and nm_tool) else 0
    type_lang = (kind << 8) | (arch << 4) | 1
# High byte: attributes (re-entrant). Low byte: ABI version, which the kernel
# compares against its own and rejects on a mismatch -- otherwise a module
# built against an old interface runs until it fails somewhere obscure.
#
# Read out of the header rather than written here. It was written here, and the
# first time the version moved, every module was built claiming the old one and
# the kernel refused all fifty-two of them -- a whole machine that booted to
# "Nothing to run". A number that must agree with another number belongs in one
# place, and the other place asks.
    UBIQOS_ABI_VERSION = abi_constant("UBIQOS_ABI_VERSION")
    # Bit 0 re-entrant, bit 1 real-time. A real-time module keeps its code and
    # its process memory in SRAM; everything else is given PSRAM, which is
    # plentiful but sits behind the XIP cache with latency nobody can predict.
    # Bit 0 is re-entrant, as in OS-9: one copy of the code, one data area per
    # process. A module without it has writable data shared between instances,
    # so the kernel allows only one instance to exist.
    attrs = (0 if single else 1) | (2 if realtime else 0)
    # Bit 3: start this program when the system comes up. See
    # UBIQOS_ATTR_AUTOSTART; only a program can be started, so only a program
    # may ask.
    if autostart:
        if module_type != "program":
            sys.exit(f"{module_name}: --autostart is for programs, not a {module_type}")
        attrs |= 8
    # Bit 4: plain data, not a device descriptor -- see UBIQOS_ATTR_PLAIN.
    if plain:
        if module_type != "data":
            sys.exit(f"{module_name}: --plain is for data modules, not a {module_type}")
        attrs |= 0x10

    # Three reasons for one answer: writable data, addresses to fix, or a .bss
    # to zero. Any of them means the module cannot run where it lies.
    if reloc or bss_size or (elf_path and nm_tool and has_writable_data(elf_path, nm_tool)):
        attrs |= 4
    attr_rev  = (attrs << 8) | UBIQOS_ABI_VERSION
# Total RAM: data area at the bottom and the process stack from the top. One
# trap frame is 128 bytes, so 4 kB leaves ample depth for call chains -- and it
# was every module's ration until UBIQOS_MEM_SIZE let one say otherwise. A
# module that needs a buffer larger than its stack can spare is what this is
# for; stdio's is the first.
    mem_size = 4096
    if elf_path and nm_tool:
        mem_size = find_mem_size(elf_path, nm_tool, 4096)

# The checksum covers the first nine 32-bit words AS THEY LIE IN MEMORY, and is
# stored in the tenth. Two bugs lived here: type_lang precedes attr_rev in a
# little-endian struct, hence (attr_rev << 16) | type_lang and not the other way
# round; and the kernel once summed the crc field into its own checksum.
    fields = [
        UBIQOS_SYNC, module_size, name_offset,
        (attr_rev << 16) | type_lang,
        exec_offset, mem_size,
        tls_offset, tls_init, tls_total,
        (0 << 16) | revision,
        reloc_offset, reloc_count, bss_size
    ]
    header_crc = (~sum(fields)) & 0xFFFFFFFF

    header_bytes = struct.pack('<IIIHHIIIIIHHIIII',
        UBIQOS_SYNC, module_size, name_offset,
        type_lang, attr_rev, exec_offset, mem_size,
        tls_offset, tls_init, tls_total,
        revision, 0, reloc_offset, reloc_count, bss_size, header_crc
    )
    assert len(header_bytes) == header_size, "header is not %d bytes" % header_size

    with open(output_mod_path, "wb") as f:
        f.write(header_bytes)
        f.write(code_bytes)
        f.write(name_bytes)
        # Site and kind in one word, target in the next. A module is far short
        # of the 256 MB the site field allows, so four bits are free for the
        # kind and an entry stays eight bytes.
        for site, kind, target in reloc:
            f.write(struct.pack('<II', (kind << 28) | site, target))

    print(f"  module '{module_name}' revision {revision}, "
          f"{module_size} bytes, {tls_total} thread-local"
          f"{f', {mem_size} bytes of memory' if mem_size != 4096 else ''}"
          f"{f', {reloc_count} relocations' if reloc_count else ''}"
          f"{', real-time' if realtime else ''}"
          f"{', single instance' if single else ''}")

if __name__ == "__main__":
# --data as a flag rather than a positional argument: CMake drops empty
# positional arguments, so a data module was built as a program.
    argv = sys.argv[1:]
    # --library is a module the kernel calls rather than runs: exec_offset
    # points at a table of pointers instead of at an entry point, so the symbol
    # named on the command line is that table's.
    #
    # --driver is the same idea with a fixed shape. A library publishes whatever
    # list of functions it documents; a driver publishes the one table the I/O
    # manager already calls every device through, so the kernel knows what it is
    # getting without being told.
    module_type = ("data"    if "--data"    in argv else
                   "driver"  if "--driver"  in argv else
                   "library" if "--library" in argv else "program")
    argv = [a for a in argv if a not in ("--data", "--library", "--driver")]

    # --rev sets the module revision. The directory keeps the highest of a given
    # name, so a patched module replaces the one already there by carrying a
    # larger number and nothing else.
    realtime  = "--rt" in argv
    single    = "--single" in argv
    autostart = "--autostart" in argv
    plain     = "--plain" in argv
    argv = [a for a in argv if a not in ("--rt", "--single", "--autostart", "--plain")]

    revision = 1
    if "--rev" in argv:
        i = argv.index("--rev")
        revision = int(argv[i + 1])
        del argv[i:i + 2]

    create_module(*argv, module_type=module_type, revision=revision,
                  realtime=realtime, single=single, autostart=autostart, plain=plain)
