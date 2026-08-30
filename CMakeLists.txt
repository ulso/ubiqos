# ====================================================================================
# Select the development board for your hardware
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")

# Force the whole Pico SDK to build for the RP2350 in RISC-V mode
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico platform")

cmake_minimum_required(VERSION 3.16)

# Ladda Pico SDK
include(pico_sdk_import.cmake)

project(myrtos C CXX ASM)

pico_sdk_init()

# ============================================================================
# 1. BUILD THE OPERATING SYSTEM KERNEL (myrtos)
# ============================================================================
add_executable(os_kernel 
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
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
    # -N keeps the linker from page-aligning the writable sections. Without it
    # .tdata landed a page beyond the code and objcopy padded the gap, which put
    # four kilobytes of zeroes in every module that had a thread-local variable.
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv -Wl,-N ${single_link})
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

# A data module has no entry point: it IS its bytes.
function(myrtos_add_data_module name source)
    add_executable(${name}_app ${source})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2)
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv -Wl,-e,0)

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
myrtos_add_module(cxxdemo modules/cxxdemo/cxxdemo.cpp)
myrtos_add_module(lsmod modules/lsmod/lsmod.c)
myrtos_add_module(free modules/freemem/free.c)
myrtos_add_module(echo modules/echo/echo.c)
myrtos_add_module(usbecho modules/usbecho/usbecho.c)
myrtos_add_module(sh modules/sh/sh.c RT)
myrtos_add_module(ls modules/ls/ls.c)
myrtos_add_module(cat modules/cat/cat.c)
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
myrtos_add_module(keys modules/keys/keys.c RT)
myrtos_add_module(memtest modules/memtest/memtest.c)
myrtos_add_module(pimpl modules/pimpl/pimpl.cpp modules/pimpl/bump.cpp)
myrtos_add_module(bootsel modules/bootsel/bootsel.c)
myrtos_add_data_module(termdesc modules/term_desc/term_desc.c)
myrtos_add_data_module(usbdesc modules/usb_desc/usb_desc.c)
myrtos_add_data_module(kbddesc modules/kbd_desc/kbd_desc.c)
myrtos_add_data_module(condesc modules/con_desc/con_desc.c)

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
            ${CMAKE_CURRENT_BINARY_DIR}/keys.mod
            ${CMAKE_CURRENT_BINARY_DIR}/memtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/pimpl.mod
            ${CMAKE_CURRENT_BINARY_DIR}/bootsel.mod
            ${CMAKE_CURRENT_BINARY_DIR}/termdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/kbddesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/condesc.mod)
