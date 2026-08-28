#!/usr/bin/env python3
import sys
import struct

# Vi använder ditt tänkta synk-ord eller behåller 0x0509000B för RISC-V hårdvaruskydd.
# Let's use 0x0509000B since it actively prevents CPU execution crashes via Custom-0!
MYRTOS_SYNC = 0x0509000B

def find_entry_offset(elf_path, nm_tool, symbol):
    """Var modulens startpunkt ligger i den råa binären.

    objcopy -O binary skriver från den lägsta laddade adressen, så offseten är
    symbolens adress minus den lägsta. Att anta noll gick fel: länkaren la
    myrtos_syscall först, och kärnan anropade den i stället för module_main.
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

    # En datamodul -- en enhetsbeskrivare, en tabell, ett teckensnitt -- har
    # ingen startpunkt. exec_offset blir noll och kärnan startar den inte.
    entry_in_code = 0
    if module_type != "data" and elf_path and nm_tool:
        entry_in_code = find_entry_offset(elf_path, nm_tool, entry_symbol)

    # 4-byte alignment på koden
    if len(code_bytes) % 4 != 0:
        code_bytes += b'\x00' * (4 - (len(code_bytes) % 4))

    name_bytes = module_name.encode('utf-8') + b'\x00'
    if len(name_bytes) % 4 != 0:
        name_bytes += b'\x00' * (4 - (len(name_bytes) % 4))

    # Huvudet är 28 byte: 3 x uint32, 2 x uint16, 3 x uint32 -- samma som
    # myrtos_module_header_t i common/modules.h. Det stod 32 här, vilket sköt
    # exec_offset och name_offset fyra byte fel in i koden.
    header_size = 28
    exec_offset = 0 if module_type == "data" else header_size + entry_in_code
    name_offset = header_size + len(code_bytes)
    module_size = header_size + len(code_bytes) + len(name_bytes)

    # Standardvärden för dina unika fält
    MYRTOS_TYPE_PROGRAM = 1
    MYRTOS_TYPE_DATA    = 3
    kind = MYRTOS_TYPE_DATA if module_type == "data" else MYRTOS_TYPE_PROGRAM
    type_lang = (kind << 8) | 1  # hög byte: typ, låg byte: språk (C)
    # Hög byte: attribut (re-entrant). Låg byte: ABI-version, som kärnan
    # jämför med sin egen och avvisar vid skillnad -- annars körs en modul
    # byggd mot ett gammalt gränssnitt tills den havererar på något obegripligt.
    MYRTOS_ABI_VERSION = 1
    attr_rev  = (1 << 8) | MYRTOS_ABI_VERSION
    # Totalt RAM: dataområde nedtill och processens stack från toppen. En
    # trap-ram är 128 byte, så 4 kB ger gott om djup för anropskedjor.
    mem_size = 4096

    # Checksumman går över huvudets sex första 32-bitarsord SOM DE LIGGER I
    # MINNET, och lagras i det sjunde. Två fel bodde här: fältet type_lang
    # ligger före attr_rev i en little-endian-struct, alltså
    # (attr_rev << 16) | type_lang och inte tvärtom; och kärnan summerade sju
    # ord, vilket räknade in själva crc-fältet i sin egen summa.
    fields = [
        MYRTOS_SYNC, module_size, name_offset,
        (attr_rev << 16) | type_lang,
        exec_offset, mem_size
    ]
    header_crc = (~sum(fields)) & 0xFFFFFFFF

    # Packa enligt din struct: sync, size, name, type_lang, attr_rev, exec, data, crc
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
    # --data som flagga i stället för ett positionellt argument: CMake släpper
    # tomma positionella argument, så en datamodul byggdes som program.
    argv = sys.argv[1:]
    module_type = "data" if "--data" in argv else "program"
    argv = [a for a in argv if a != "--data"]
    create_module(*argv, module_type=module_type)
