# The Pico VS Code extension wrote the block below when the project was imported,
# and it is kept because it earns its place: pico-vscode.cmake sets PICO_SDK_PATH
# and, seeing RISCV in the toolchain name, PICO_PLATFORM -- on every configure,
# from a file that is in the repository. That is exactly what a stale cache
# holding a wrong SDK path could not survive, and one of those cost an afternoon.
#
# One line is edited despite what the banner says. The import set toolchainVersion
# to 15_2_Rel1, which is arm-none-eabi and cannot build this at all. If an
# extension update ever writes it back, that is what to look for.
# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion RISCV_ZCB_RPI_2_2_0_3)   # NOT 15_2_Rel1: that one is arm-none-eabi
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# ====================================================================================
# Select the development board for your hardware
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")

# Force the whole Pico SDK to build for the RP2350 in RISC-V mode
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico platform")

cmake_minimum_required(VERSION 3.16)

# Release, forced, and not by preference. A debug build of the kernel wants
# about eight kilobytes more RAM than the machine has -- the framebuffer is
# 307200 bytes of a 520 kB part and this is linked copy_to_ram, so -Og -g does
# not fit. It fails at the link with "region RAM overflowed" and nothing to say
# the build type was the cause, which is a poor thing to leave lying around for
# whoever presses the wrong button in an IDE.
#
# FORCE because a tool that asks for Debug is asking for something that cannot
# link, and it is better to give it the build that works than the error that
# does not explain itself. When the display stops being one fixed mode and gives
# back a quarter megabyte, this comes out.
set(CMAKE_BUILD_TYPE Release CACHE STRING "the only one that fits" FORCE)

# The compile database, which is what an editor should read rather than being
# told the include paths a second time by hand. It gets the exact flags each
# file was built with -- every -I, every -D, the right architecture -- and it
# cannot drift out of step with the build, because it is the build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(pico_sdk_import.cmake)

project(myrtos C CXX ASM)

pico_sdk_init()

# ============================================================================
# 1. BUILD THE OPERATING SYSTEM KERNEL (myrtos)
# ============================================================================
add_executable(os_kernel 
    kernel/crashlog.c
    kernel/vfs.c
    kernel/tmpfs.c
    kernel/varfs.c
    kernel/main.c 
    kernel/tlsf.c
    kernel/scheduler.c 
    kernel/syscalls.c 
    kernel/io.c
    kernel/sdcard.c
    kernel/fat32.c
    kernel/moddir.c
    kernel/flashmod.c
    kernel/usbdev.c
    kernel/usb_descriptors.c
    kernel/scheduler.S
    kernel/pio/pioprobe.c
    kernel/usbhost.c
    kernel/video.c
    kernel/console.c
    kernel/fsserver.c
    kernel/printf.c
    kernel/wifi.c
    third_party/pico_sd_card/sd_card.c
    kernel/font8x16.c
    kernel/font6x12.c

    # Pico-PIO-USB bit-bangs a USB host on two PIO state machines. It is pinned
    # as a submodule so the version is written down: the copy that was lying
    # around was eleven months old and missing an out-of-bounds memcpy fix and
    # the RP2350-E9 errata conditional.
    lib/Pico-PIO-USB/src/pio_usb.c
    lib/Pico-PIO-USB/src/pio_usb_host.c
    lib/Pico-PIO-USB/src/pio_usb_device.c
    lib/Pico-PIO-USB/src/usb_crc.c

    # TinyUSB's host side, and the glue that drives it from PIO rather than from
    # the hardware controller -- which is busy being our console.
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/usbh.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/cdc/cdc_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# Execution happens in RAM, but the image lives in flash and is copied there at
# boot. That is deliberate: the boot ROM reads PICOBIN_IMAGE_TYPE_EXE_CPU from
# the image and only then switches to RISC-V. With no_flash there is no image to
# switch on, so the chip starts in ARM and the debugger's reset-and-halt finds
# no RISC-V core.
pico_set_binary_type(os_kernel copy_to_ram)

pico_generate_pio_header(os_kernel ${CMAKE_CURRENT_SOURCE_DIR}/kernel/pio/probe.pio)

# The SD card over SDIO, four data lines on PIO, vendored from pico-extras. The
# board wires DAT0..DAT3 to GP36..GP39 and the board header already names them,
# so the driver's own defaults are the right ones here.
#
# It prints on its error paths. Rather than edit somebody else's source, its
# printf is pointed at ours -- see kernel/printf.c.
pico_generate_pio_header(os_kernel ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/sd_card.pio)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/sd_card.c
    PROPERTIES COMPILE_DEFINITIONS "printf=myrtos_printf")

# Link against the Pico standard libraries and enable PSRAM support.
# pico_standard_link provides only linker scripts and link flags. The startup
# code crt0, which defines _entry_point and calls main, comes with pico_runtime.
# hardware_uart and hardware_gpio are for the diagnostic port.
target_link_libraries(os_kernel pico_runtime hardware_uart hardware_gpio hardware_spi pico_time
    pico_bootrom hardware_pio hardware_dma hardware_clocks pico_stdlib hardware_psram
    tinyusb_device)

# TinyUSB looks for tusb_config.h on the include path.
target_include_directories(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/Pico-PIO-USB/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/include)

# Enable extra warnings and optimisation for the RTOS
target_compile_options(os_kernel PRIVATE -Wall -O2)

# Route every panic through __wrap_panic, so the reason survives the crash.
# See kernel/crashlog.c.
target_link_options(os_kernel PRIVATE -Wl,--wrap=panic)

# Produce the uf2 file and friends for loading onto the Fruit Jam
pico_add_extra_outputs(os_kernel)

# ============================================================================
# MODULBYGGE
# ============================================================================
# The .mod file is a real OUTPUT rather than a POST_BUILD step. The difference
# is that it is rebuilt when make_module.py changes too, not only when the
# module's source does.
# Extra sources may follow the first: a module can be split across files like
# any other program. What it may not do is keep state in a file-scope variable,
# static or not -- one copy of the code is shared by every process running it,
# so the variable would be shared too. See docs/writing-modules.md.
# A trailing RT marks the module real-time: its code and its process memory
# stay in SRAM instead of going to PSRAM. Anything whose timing matters wants
# it -- the shell, and whatever reads a device that will not wait.
function(myrtos_add_module name source)
    set(rt_flag "")
    if("RT" IN_LIST ARGN)
        set(rt_flag "--rt")
        list(REMOVE_ITEM ARGN "RT")
    endif()
    # A trailing SINGLE marks the module not re-entrant: its writable data is
    # shared, so only one instance may run. A service that owns hardware or a
    # protocol stack is one of these by nature.
    set(single_flag "")
    set(single_link "")
    if("SINGLE" IN_LIST ARGN)
        set(single_flag "--single")
        # Linked AT the address it will be loaded at, which is what lets it have
        # absolute addresses and writable data at all. -fno-zero-initialized-in-bss
        # puts the zeroed globals in .data so they are in the image: objcopy drops
        # .bss, and a loader that copies module_size bytes would leave them out.
        set(single_link "-Wl,-Ttext=0x1178002c")   # MYRTOS_SINGLE_TEXT
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()
    add_executable(${name}_app ${source} ${ARGN})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2
        # -fno-use-cxa-atexit puts the destructors of globals in .fini_array
        # instead of registering them with __cxa_atexit, which would want that
        # function and __dso_handle from a C library that is not here. Measured:
        # with the flag the object has no undefined symbols at all.
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics
                                  -fno-use-cxa-atexit>)
    # -N keeps the linker from page-aligning the writable sections. Without it
    # .tdata landed a page beyond the code and objcopy padded the gap, which put
    # four kilobytes of zeroes in every module that had a thread-local variable.
    # -e,0 and --no-warn-rwx-segments say nothing about the code; they say that
    # two warnings do not apply to a module. It has no _start, because the
    # kernel enters it at exec_offset -- which make_module.py finds with nm, not
    # from the ELF entry -- and its one loadable segment is read, write and
    # execute together, because that is what a module is. Sixty-six lines of
    # them per clean build is how a real warning goes unnoticed.
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv -Wl,-N
                        -Wl,-e,0 -Wl,--no-warn-rwx-segments ${single_link})
    if(single_flag)
        target_compile_options(${name}_app PRIVATE -fno-zero-initialized-in-bss)
    endif()

# Position independence is checked before the module is packaged. An absolute
# address in an allocated section -- a static function pointer, a vtable, a
# pointer to a static variable -- means the module cannot be loaded at an
# unknown address, and that does not show in the source.
    add_custom_command(TARGET ${name}_app POST_BUILD
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${single_flag}
                ${CMAKE_READELF}
                $<TARGET_OBJECTS:${name}_app>
                $<TARGET_FILE:${name}_app>
        COMMAND_EXPAND_LISTS
        COMMENT "Checking that ${name} is position independent")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary
                $<TARGET_FILE:${name}_app>
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        DEPENDS ${name}_app
        COMMENT "Extracting raw position-independent machine code from ${name}")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
                ${single_flag}
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}"
                $<TARGET_FILE:${name}_app>
                ${CMAKE_NM}
                module_main
                ${rt_flag}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
        COMMENT "Building module ${name}.mod")
endfunction()

# --- The second toolchain -------------------------------------------------
# Clang and lld, beside GCC rather than instead of it, chosen per module.
#
# There is one reason to want it and it is C++ with virtual functions. A vtable
# is a table of pointers, which is the one thing a shareable module may not
# contain, so every virtual call has been out of reach. Clang's
# -fexperimental-relative-c++-abi-vtables stores the distance from each slot to
# its function instead of the address, which is exactly what the module format
# needs -- and GNU ld cannot link the result, so it is clang *and* lld or
# neither. See the closing sections of docs/writing-modules.md for the
# measurements.
#
# CMake has one compiler per project, so these modules are built by hand with
# custom commands rather than add_executable. They go through the same
# check_module.py, the same objcopy and the same make_module.py, and land as the
# same ${name}.mod -- nothing downstream can tell which compiler made them.
find_program(MYRTOS_CLANG   clang   HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(MYRTOS_CLANGXX clang++ HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(MYRTOS_LLD     ld.lld  HINTS /opt/homebrew/bin /opt/homebrew/opt/lld/bin /usr/local/bin)

# The architecture comes from whatever the SDK's toolchain file chose, so the
# two compilers cannot drift apart. Clang may not know every extension GCC does,
# which is why this is probed rather than assumed: a machine without clang, or
# with one that cannot build for this target, quietly gets the GCC path instead
# of a broken build.
separate_arguments(MYRTOS_TARGET_ARCH NATIVE_COMMAND "${CMAKE_C_FLAGS}")
set(MYRTOS_CLANG_OK FALSE)
if(MYRTOS_CLANG AND MYRTOS_CLANGXX AND MYRTOS_LLD)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.c "int probe(void){return 0;}\n")
    execute_process(
        COMMAND ${MYRTOS_CLANG} --target=riscv32-unknown-elf ${MYRTOS_TARGET_ARCH}
                -ffreestanding -nostdlib -c ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.c
                -o ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.o
        RESULT_VARIABLE probe_rc OUTPUT_QUIET ERROR_VARIABLE probe_err)
    if(probe_rc EQUAL 0)
        set(MYRTOS_CLANG_OK TRUE)
        message(STATUS "Clang module toolchain: ${MYRTOS_CLANG} + ${MYRTOS_LLD}")
    else()
        message(STATUS "Clang found but cannot build for this target; modules fall back to gcc")
        message(STATUS "  ${probe_err}")
    endif()
else()
    message(STATUS "No clang/lld found; clang modules fall back to gcc")
endif()

# -std=gnu23 because clang defaults to a C where bool is not a keyword.
# -fno-jump-tables because a large switch otherwise becomes a table of absolute
# addresses -- sh comes out with 97 R_RISCV_32 without it and none with it.
# GCC is given neither and has simply never emitted a jump table here, which is
# luck rather than design; check_module.py would catch it either way.
set(MYRTOS_CLANG_C_FLAGS   -std=gnu23 -fno-jump-tables)
set(MYRTOS_CLANG_CXX_FLAGS -fno-exceptions -fno-rtti -fno-threadsafe-statics
                           -fno-use-cxa-atexit
                           -fno-jump-tables -fexperimental-relative-c++-abi-vtables)

# Same signature and same RT / SINGLE options as myrtos_add_module. A module
# built this way is not different in kind; it was only compiled by the other
# compiler.
function(myrtos_add_clang_module name source)
    if(NOT MYRTOS_CLANG_OK)
        myrtos_add_module(${name} ${source} ${ARGN})
        return()
    endif()

    set(rt_flag "")
    if("RT" IN_LIST ARGN)
        set(rt_flag "--rt")
        list(REMOVE_ITEM ARGN "RT")
    endif()
    set(single_flag "")
    set(single_link "")
    set(single_compile "")
    if("SINGLE" IN_LIST ARGN)
        set(single_flag "--single")
        set(single_link "-Ttext=0x1178002c")   # MYRTOS_SINGLE_TEXT
        set(single_compile "-fno-zero-initialized-in-bss")
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()

    set(objdir ${CMAKE_CURRENT_BINARY_DIR}/${name}_clang)
    file(MAKE_DIRECTORY ${objdir})
    set(objs "")
    foreach(src ${source} ${ARGN})
        get_filename_component(base ${src} NAME_WE)
        get_filename_component(ext ${src} EXT)
        set(obj ${objdir}/${base}.o)
        if(ext STREQUAL ".cpp" OR ext STREQUAL ".cc" OR ext STREQUAL ".cxx")
            set(cc ${MYRTOS_CLANGXX})
            set(lang_flags ${MYRTOS_CLANG_CXX_FLAGS})
        else()
            set(cc ${MYRTOS_CLANG})
            set(lang_flags ${MYRTOS_CLANG_C_FLAGS})
        endif()
        add_custom_command(
            OUTPUT ${obj}
            COMMAND ${cc} --target=riscv32-unknown-elf ${MYRTOS_TARGET_ARCH}
                    -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2
                    ${lang_flags} ${single_compile}
                    -c ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
            COMMENT "Compiling ${src} with clang")
        list(APPEND objs ${obj})
    endforeach()

    # lld needs no --no-warn-rwx-segments: it does not warn about them. -N and
    # -e 0 mean what they mean for GNU ld -- see the note in myrtos_add_module.
    set(elf ${objdir}/${name}.elf)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${MYRTOS_LLD} -m elf32lriscv -N -e 0 ${single_link} -o ${elf} ${objs}
        DEPENDS ${objs}
        COMMENT "Linking ${name} with lld")

    add_custom_command(
        OUTPUT ${objdir}/${name}.checked
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${single_flag} ${CMAKE_READELF} ${objs} ${elf}
        COMMAND ${CMAKE_COMMAND} -E touch ${objdir}/${name}.checked
        DEPENDS ${elf} ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
        COMMENT "Checking that ${name} is position independent")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary ${elf} ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        DEPENDS ${objdir}/${name}.checked
        COMMENT "Extracting raw position-independent machine code from ${name}")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
                ${single_flag}
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}"
                ${elf}
                ${CMAKE_NM}
                module_main
                ${rt_flag}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
        COMMENT "Building module ${name}.mod (clang)")
endfunction()

# A data module has no entry point: it IS its bytes.
function(myrtos_add_data_module name source)
    add_executable(${name}_app ${source})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2)
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv -Wl,-e,0
                        -Wl,--no-warn-rwx-segments)

    add_custom_command(TARGET ${name}_app POST_BUILD
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${single_flag}
                ${CMAKE_READELF}
                $<TARGET_OBJECTS:${name}_app>
                $<TARGET_FILE:${name}_app>
        COMMAND_EXPAND_LISTS
        COMMENT "Checking that ${name} is position independent")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary
                $<TARGET_FILE:${name}_app> ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        DEPENDS ${name}_app)

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
                ${single_flag}
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}" --data
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
        COMMENT "Building data module ${name}.mod")
endfunction()

myrtos_add_module(counter modules/counter/counter.c)
myrtos_add_clang_module(cxxdemo modules/cxxdemo/cxxdemo.cpp)
myrtos_add_module(lsmod modules/lsmod/lsmod.c)
myrtos_add_module(free modules/freemem/free.c)
myrtos_add_module(echo modules/echo/echo.c)
myrtos_add_module(usbecho modules/usbecho/usbecho.c)
myrtos_add_module(sh modules/sh/sh.c RT)
myrtos_add_module(ls modules/ls/ls.c)
myrtos_add_module(cat modules/cat/cat.c)
myrtos_add_module(head modules/head/head.c)
myrtos_add_module(more modules/more/more.c)
myrtos_add_module(tlsftest modules/tlsftest/tlsftest.c)
myrtos_add_module(libctest modules/libctest/libctest.c)
myrtos_add_module(rm modules/rm/rm.c)
myrtos_add_module(cp modules/cp/cp.c)
myrtos_add_module(write modules/write/write.c)
myrtos_add_module(fill modules/fill/fill.c)
myrtos_add_module(sleep modules/sleep/sleep.c)
myrtos_add_module(spin modules/spin/spin.c)
myrtos_add_module(spinrt modules/spin/spin.c RT)
myrtos_add_module(nice modules/nice/nice.c)
myrtos_add_module(ps modules/ps/ps.c)
myrtos_add_module(mkdir modules/mkdir/mkdir.c)
# The TCP/IP stack, NOT built yet -- see docs/lwip/README.md.
#
# It compiles, links and very nearly passes the position-independence check: 57
# absolute addresses remain, in five tables that can be dealt with the way tcp.c
# already was. What stops it is not that.
#
# A flash-resident module executes in place at 0x10100000, so its .data would be
# IN FLASH and writes to it would vanish. The check's refusal of writable
# sections is a hardware fact, not a matter of style, and SINGLE was written
# without asking why the rule was there. lwIP's globals have to reach the
# process's own data area, which means thread-local storage, which means either
# a loader that copies a SINGLE module into RAM or a way to put a whole
# translation unit's globals in .tdata.
#
# Left here, in one piece and building nothing, so the next attempt starts from a
# port that works rather than from an empty directory.
#
# # The TCP/IP stack. SINGLE because lwIP keeps its tables in globals and there is
# # one of it; see docs/lwip. Two files of lwIP carry a local change for position
# # independence, and the port -- lwipopts.h, arch/cc.h, sys_arch.c and a small
# # string library, since a module is linked -nostdlib -- lives with the module.
myrtos_add_module(lwipd modules/lwipd/lwipd.c
    modules/lwipd/sys_arch.c
    modules/lwipd/mini_string.c
    modules/lwipd/mini_libc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/def.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/dns.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/inet_chksum.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/memp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/netif.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/pbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/raw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/sys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/tcp_in.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/tcp_out.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/timeouts.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/udp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/acd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/autoip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/dhcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/etharp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/icmp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/igmp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/ip4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/ip4_addr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/core/ipv4/ip4_frag.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/netif/ethernet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/apps/mdns/mdns.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/apps/mdns/mdns_domain.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/apps/mdns/mdns_out.c
    SINGLE)
target_include_directories(lwipd_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/lwipd
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lwip/include)
#

myrtos_add_module(wifi modules/wifi/wifi.c)
myrtos_add_module(pwd modules/pwd/pwd.c)
myrtos_add_module(rmdir modules/rmdir/rmdir.c)
myrtos_add_module(mount modules/mount/mount.c)
myrtos_add_module(msgtest modules/msgtest/msgtest.c SINGLE)
myrtos_add_module(msgcli modules/msgcli/msgcli.c)
myrtos_add_module(font modules/font/font.c)
myrtos_add_module(color modules/color/color.c)
myrtos_add_module(kill modules/kill/kill.c)
myrtos_add_module(cu modules/cu/cu.c)
myrtos_add_module(keys modules/keys/keys.c RT)
myrtos_add_module(memtest modules/memtest/memtest.c)
myrtos_add_module(pimpl modules/pimpl/pimpl.cpp modules/pimpl/bump.cpp)
myrtos_add_module(bootsel modules/bootsel/bootsel.c)
myrtos_add_data_module(termdesc modules/term_desc/term_desc.c)
myrtos_add_data_module(usbdesc modules/usb_desc/usb_desc.c)
myrtos_add_data_module(kbddesc modules/kbd_desc/kbd_desc.c)
myrtos_add_data_module(condesc modules/con_desc/con_desc.c)
myrtos_add_data_module(acmdesc modules/acm_desc/acm_desc.c)

add_custom_target(modules ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/counter.mod
            ${CMAKE_CURRENT_BINARY_DIR}/cxxdemo.mod
            ${CMAKE_CURRENT_BINARY_DIR}/lsmod.mod
            ${CMAKE_CURRENT_BINARY_DIR}/free.mod
            ${CMAKE_CURRENT_BINARY_DIR}/echo.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbecho.mod
            ${CMAKE_CURRENT_BINARY_DIR}/sh.mod
            ${CMAKE_CURRENT_BINARY_DIR}/ls.mod
            ${CMAKE_CURRENT_BINARY_DIR}/cat.mod
            ${CMAKE_CURRENT_BINARY_DIR}/head.mod
            ${CMAKE_CURRENT_BINARY_DIR}/more.mod
            ${CMAKE_CURRENT_BINARY_DIR}/tlsftest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/libctest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/rm.mod
            ${CMAKE_CURRENT_BINARY_DIR}/cp.mod
            ${CMAKE_CURRENT_BINARY_DIR}/write.mod
            ${CMAKE_CURRENT_BINARY_DIR}/fill.mod
            ${CMAKE_CURRENT_BINARY_DIR}/sleep.mod
            ${CMAKE_CURRENT_BINARY_DIR}/spin.mod
            ${CMAKE_CURRENT_BINARY_DIR}/spinrt.mod
            ${CMAKE_CURRENT_BINARY_DIR}/nice.mod
            ${CMAKE_CURRENT_BINARY_DIR}/ps.mod
            ${CMAKE_CURRENT_BINARY_DIR}/mkdir.mod
            ${CMAKE_CURRENT_BINARY_DIR}/pwd.mod
            ${CMAKE_CURRENT_BINARY_DIR}/wifi.mod
            ${CMAKE_CURRENT_BINARY_DIR}/lwipd.mod
            ${CMAKE_CURRENT_BINARY_DIR}/rmdir.mod
            ${CMAKE_CURRENT_BINARY_DIR}/mount.mod
            ${CMAKE_CURRENT_BINARY_DIR}/msgtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/msgcli.mod
            ${CMAKE_CURRENT_BINARY_DIR}/font.mod
            ${CMAKE_CURRENT_BINARY_DIR}/color.mod
            ${CMAKE_CURRENT_BINARY_DIR}/kill.mod
            ${CMAKE_CURRENT_BINARY_DIR}/cu.mod
            ${CMAKE_CURRENT_BINARY_DIR}/keys.mod
            ${CMAKE_CURRENT_BINARY_DIR}/memtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/pimpl.mod
            ${CMAKE_CURRENT_BINARY_DIR}/bootsel.mod
            ${CMAKE_CURRENT_BINARY_DIR}/termdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/kbddesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/condesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/acmdesc.mod)

# What lives in flash rather than on the card.
#
# A module in flash runs where it lies. One read from the card is copied into
# RAM at startup and stays there, so the card is for what is being worked on,
# and this is for the system. The order is the order of the image, which is the
# order lsmod reports; it has no other meaning.
#
# lwipd is deliberately absent: it is a SINGLE module linked to a fixed address
# and belongs nowhere near a concatenated image.
set(MYRTOS_RESIDENT
    tlsftest
    more
    libctest
    head
    # Descriptors first. They are what the devices are made of, and a directory
    # that fills up turns away whatever comes last -- which twice now has been
    # these, leaving a machine with no console to print the complaint on.
    termdesc
    usbdesc
    kbddesc
    condesc
    acmdesc

    sh
    ls
    cat
    cp
    rm
    write
    fill
    mkdir
    rmdir
    pwd
    mount
    wifi
    font
    color
    kill
    cu
    sleep
    spin
    spinrt
    nice
    ps
    keys
    bootsel
    memtest
    pimpl
    echo
    lsmod
    free
    msgtest
    msgcli
)
set(resident_mods "")
foreach(m ${MYRTOS_RESIDENT})
    list(APPEND resident_mods ${CMAKE_CURRENT_BINARY_DIR}/${m}.mod)
endforeach()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/modules.bin
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_flash_image.py
            ${CMAKE_CURRENT_BINARY_DIR}/modules.bin ${resident_mods}
    DEPENDS ${resident_mods} ${CMAKE_CURRENT_SOURCE_DIR}/make_flash_image.py
    COMMENT "Building the resident module image")

add_custom_target(flashimage ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/modules.bin)
add_dependencies(flashimage modules)

# The C library's heap is whatever is left of SRAM once this copy_to_ram build
# has been laid down, and nothing in the toolchain complains when that is
# nothing: the SDK's linker script asserts only that the stack and the heap do
# not overlap, which zero bytes of heap satisfies. The kernel then panics during
# pre-init, before the console exists to say so.
#
# So the build says it instead. See the note above MYRTOS_HEAP_SIZE in main.c.
add_custom_command(TARGET os_kernel POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DELF=$<TARGET_FILE:os_kernel>
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_heap.cmake
    VERBATIM)
