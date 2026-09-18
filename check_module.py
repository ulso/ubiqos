#!/usr/bin/env python3
"""Check that a module really is position independent and shareable.

Two properties decide whether a module can be loaded at an unknown address and
shared between processes, and neither shows in the source:

  1. No absolute address the loader cannot fix. PC-relative jumps and strings
     travel with the module wherever it goes and need nothing. A pointer in a
     table does not travel -- it is an absolute address the linker wrote in --
     but it is a 32-bit word in data, and the loader relocates those: they are
     counted here and listed in the module's relocation table. Anything else
     absolute has no such answer and is still refused.

  2. No writable sections. If .data or .bss are present in the module image,
     two processes sharing the code write to the same variables.

Relocations in the debug sections do not count: they never travel along when
objcopy extracts the raw binary.
"""
import subprocess, sys, re

# A module marked SINGLE is not re-entrant: it may only ever run once, so its
# writable data is shared with nobody and the objection below does not apply.
# That is what OS-9's re-entrant attribute meant, and it is what lets a service
# like a protocol stack keep its globals where its authors put them.
# A module marked SINGLE is linked at a fixed address and may only run once, so
# neither objection below applies to it: absolute addresses are correct because
# the address is known, and its writable data is shared with nobody.
# A module marked SINGLE still has writable data of its own, which is what the
# attribute means. What it no longer has is a fixed address, so the relocation
# check below applies to it exactly as to any other module.
argv = [a for a in sys.argv if a != "--single"]
single = len(argv) != len(sys.argv)

readelf, obj_files, elf = argv[1], argv[2:-1], argv[-1]

# PC-relative and purely local types. Anything else in an allocated section is
# an absolute address.
POSITION_INDEPENDENT = {
    # Thread-local accesses are offsets from tp, fixed at link time and carrying
    # no absolute address. They are how a module keeps per-process variables.
    "R_RISCV_TPREL_HI20", "R_RISCV_TPREL_LO12_I", "R_RISCV_TPREL_LO12_S",
    "R_RISCV_TPREL_ADD", "R_RISCV_TLS_TPREL32",
    "R_RISCV_PCREL_HI20", "R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
    "R_RISCV_BRANCH", "R_RISCV_JAL", "R_RISCV_RVC_BRANCH", "R_RISCV_RVC_JUMP",
    "R_RISCV_CALL", "R_RISCV_CALL_PLT", "R_RISCV_RELAX", "R_RISCV_ALIGN",
    "R_RISCV_ADD8", "R_RISCV_ADD16", "R_RISCV_ADD32", "R_RISCV_ADD64",
    "R_RISCV_SUB6", "R_RISCV_SUB8", "R_RISCV_SUB16", "R_RISCV_SUB32",
    "R_RISCV_SUB64", "R_RISCV_SET6", "R_RISCV_SET8", "R_RISCV_SET16",
    "R_RISCV_SET32", "R_RISCV_SET_ULEB128", "R_RISCV_SUB_ULEB128",
    # What clang's -fexperimental-relative-c++-abi-vtables emits. The vtable
    # slot holds S + A - P, the distance from the slot to the function, so the
    # table is correct wherever the module lands. Verified rather than assumed:
    # linked with lld, the three slots read 0x62, 0x66, 0x6a where the absolute
    # build has 0x000100f8, 0x000100fc, 0x00010100.
    "R_RISCV_PLT32",

    # And ARM's, which are fewer because Thumb-2 reaches globals through
    # literal pools rather than through instruction pairs: the absolute address
    # ends up as a word in .text, not as bits inside two instructions.
    "R_ARM_NONE", "R_ARM_V4BX",
    "R_ARM_REL32", "R_ARM_PREL31",
    "R_ARM_CALL", "R_ARM_JUMP24",
    "R_ARM_THM_CALL", "R_ARM_THM_JUMP24", "R_ARM_THM_JUMP19",
    "R_ARM_THM_JUMP11", "R_ARM_THM_JUMP8",
    "R_ARM_THM_PC12", "R_ARM_THM_PC8", "R_ARM_THM_PC11",

    # Thread-local, in the local-exec model: the offset of the variable within
    # the block the thread pointer names, fixed at link time and carrying no
    # address at all. RISC-V's TPREL relocations above say the same thing.
    #
    # The other TLS models are deliberately absent. Initial-exec and
    # global-dynamic reach the variable through a GOT, which a module has no
    # room for and no loader to fill; being refused here says so at the build.
    "R_ARM_TLS_LE32",
}

# Absolute, and relocated at load time rather than refused. The loader copies
# the module and writes each of these from the target the table carries.
#
# The first is a 32-bit word in data holding an address, and for a module that
# links no library it is the only one: -mcmodel=medany makes its own code
# PC-relative. The other three are instructions reaching a global absolutely
# with lui, which is what newlib and libgcc do -- they arrive prebuilt in the
# toolchain's default code model, and nothing about a module's own flags
# changes that.
LOADER_FIXES = {"R_RISCV_32", "R_RISCV_HI20", "R_RISCV_LO12_I", "R_RISCV_LO12_S",
                # ARM needs one. A literal pool holds the address as a plain
                # word, which is the same thing R_RISCV_32 is and is fixed the
                # same way. movw/movt pairs would be ARM's answer to HI20/LO12
                # and are deliberately NOT here: the loader cannot write them
                # yet, and being refused at the build is better than being
                # accepted and left wrong.
                "R_ARM_ABS32"}

# Types this readelf cannot name, by number. Binutils prints "unrecognized: 3b"
# where the name belongs, and refusing everything it cannot name would refuse a
# relocation that is perfectly position independent. llvm-readelf names PLT32
# and binutils does not, and which readelf happens to be installed is no basis
# for deciding whether a module may be loaded.
BY_NUMBER = {"3b": "R_RISCV_PLT32"}

# Never conclude anything from output that was not produced. readelf on a file
# that is not there prints an error and exits non-zero, and reading its empty
# stdout looked exactly like a module with nothing wrong -- a link that failed
# behind a redirect was reported as position independent and shareable. Twice.
def readelf_or_die(readelf, args, path):
    r = subprocess.run([readelf, "-W"] + args + [path],
                       capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        print("MODULE CHECK FAILED: could not read %s" % path)
        if r.stderr.strip():
            print("  " + r.stderr.strip().splitlines()[0])
        sys.exit(1)
    return r.stdout

# Two properties, and they are not the same one. A module is POSITION
# INDEPENDENT when it holds no absolute addresses, so it runs wherever it is
# loaded -- that is about relocations. It is SHAREABLE when it holds no writable
# data, so one copy can serve many processes -- that is about sections. A module
# with a writable static is perfectly position independent and simply cannot be
# shared, and saying "not position independent" about it sends the reader
# looking for the wrong thing.
not_pic = []
not_shared = []
relocated = []

for obj in obj_files:
    out = readelf_or_die(readelf, ["-r"], obj)
    section = None
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            section = m.group(1)
            continue
        # The type is the third field, whatever it says. Searching for
        # R_RISCV_\w+ instead meant that a type this readelf cannot name --
        # it prints "unrecognized: 3b" -- matched nothing and the line was
        # skipped in silence. Clang's relative vtables emit R_RISCV_PLT32,
        # which is 0x3b, and sailed through a check written to refuse exactly
        # that sort of thing. A checker that ignores what it does not
        # understand is worse than no checker: it says yes with authority.
        m = re.match(r"\s*[0-9a-fA-F]{8,16}\s+[0-9a-fA-F]{8,16}\s+(\S+)\s*(\S+)?", line)
        if not m or not section:
            continue
# Debug information is never loaded.
        # .rel and .rela both, because the two machines differ: RISC-V emits
        # RELA with an explicit addend, ARM emits REL and keeps the addend in
        # the word being relocated. Matching only ".rela.debug" let every debug
        # relocation in an ARM object through, and echo alone reported six.
        if re.match(r"\.rela?\.(debug|eh_frame)", section):
            continue
        kind = m.group(1)
        if kind == "unrecognized:" and m.group(2) in BY_NUMBER:
            kind = BY_NUMBER[m.group(2)]
        if kind in LOADER_FIXES:
            relocated.append(f"{obj}: {kind} in {section}")
            continue
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
            not_pic.append(f"{obj}: {kind} in {hint}")

out = readelf_or_die(readelf, ["-S"], elf)
# By the W flag, not by a list of names. The list said .data, .bss, .sdata and
# .sbss, and it was wrong in both directions: it refused a const array that GCC
# had put in .sdata read-only -- flags "A", not "WA", because eight bytes fits
# under -msmall-data-limit -- and it would have missed a writable section under
# any name not on it. Writable is a flag; it should be read as one.
#
# .tdata and .tbss are the exception and are meant to be. They are writable, but
# one copy per process rather than one shared between them, which is exactly
# what a module needs.
SECTION = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+"
    r"([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(\S*)\s+\d+\s+\d+\s+\d+\s*$")

for line in out.splitlines():
    m = SECTION.match(line)
    if not m or single:
        continue
    name, size, flags = m.group(1), m.group(2), m.group(3)
    if name in (".tdata", ".tbss"):
        continue
    # Allocated, writable, and not code. .text comes out WAX because a module
    # is linked -N, which is what the RWX warning at every link is about -- it
    # is one loadable segment by design, not a writable variable. Data sections
    # are WA without the X, and those are the ones two processes would share.
    if "W" in flags and "A" in flags and "X" not in flags and int(size, 16) != 0:
        not_shared.append(f"{elf}: writable section {name} is present")

def report(title, items, why):
    print(title, file=sys.stderr)
    seen = set()
    for p in items:
        if p in seen: continue
        seen.add(p)
        print(f"  {p}", file=sys.stderr)
    print("\n" + why, file=sys.stderr)

if not_pic:
    report("MODULE IS NOT POSITION INDEPENDENT:", not_pic,
           "An absolute address in an allocated section is not known until the\n"
           "module is loaded. It is usually a table of pointers -- a switch or\n"
           "if-chain returning string literals, a static function pointer, or a\n"
           "C++ vtable.")

# Writable data is no longer refused. It used to be, because one copy of the
# code in flash served every process and they would have shared it; a module
# that has any is marked UBIQOS_ATTR_PRIVATE and given a copy of its own, so a
# static variable is simply a variable. It is still worth saying, because a
# module without one costs no memory at all.
if not_shared and not single:
    seen = set()
    for p in not_shared:
        if p in seen: continue
        seen.add(p)
        print("  %s -- needs a copy per process" % p)

if not_pic:
    print("See docs/writing-modules.md.", file=sys.stderr)
    sys.exit(1)

if relocated:
    print(f"  {elf}: shareable, {len(relocated)} address"
          f"{'es' if len(relocated) != 1 else ''} for the loader to fix")
else:
    print(f"  {elf}: position independent and shareable")
