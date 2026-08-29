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
)

# Execution happens in RAM, but the image lives in flash and is copied there at
# boot. That is deliberate: the boot ROM reads PICOBIN_IMAGE_TYPE_EXE_CPU from
# the image and only then switches to RISC-V. With no_flash there is no image to
# switch on, so the chip starts in ARM and the debugger's reset-and-halt finds
# no RISC-V core.
pico_set_binary_type(os_kernel copy_to_ram)

# Link against the Pico standard libraries and enable PSRAM support.
# pico_standard_link provides only linker scripts and link flags. The startup
# code crt0, which defines _entry_point and calls main, comes with pico_runtime.
# hardware_uart and hardware_gpio are for the diagnostic port.
target_link_libraries(os_kernel pico_runtime hardware_uart hardware_gpio hardware_spi pico_time
    pico_bootrom
    tinyusb_device)

# TinyUSB looks for tusb_config.h on the include path.
target_include_directories(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel)

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
function(myrtos_add_module name source)
    add_executable(${name}_app ${source} ${ARGN})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv)

# Position independence is checked before the module is packaged. An absolute
# address in an allocated section -- a static function pointer, a vtable, a
# pointer to a static variable -- means the module cannot be loaded at an
# unknown address, and that does not show in the source.
    add_custom_command(TARGET ${name}_app POST_BUILD
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
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
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}"
                $<TARGET_FILE:${name}_app>
                ${CMAKE_NM}
                module_main
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
        COMMENT "Bygger modulen ${name}.mod")
endfunction()

# A data module has no entry point: it IS its bytes.
function(myrtos_add_data_module name source)
    add_executable(${name}_app ${source})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2)
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv -Wl,-e,0)

    add_custom_command(TARGET ${name}_app POST_BUILD
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
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
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}" --data
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
        COMMENT "Bygger databmodulen ${name}.mod")
endfunction()

myrtos_add_module(counter modules/counter/counter.c)
myrtos_add_module(cxxdemo modules/cxxdemo/cxxdemo.cpp)
myrtos_add_module(lsmod modules/lsmod/lsmod.c)
myrtos_add_module(free modules/freemem/free.c)
myrtos_add_module(echo modules/echo/echo.c)
myrtos_add_module(usbecho modules/usbecho/usbecho.c)
myrtos_add_module(sh modules/sh/sh.c)
myrtos_add_module(ls modules/ls/ls.c)
myrtos_add_module(cat modules/cat/cat.c)
myrtos_add_module(rm modules/rm/rm.c)
myrtos_add_module(cp modules/cp/cp.c)
myrtos_add_module(write modules/write/write.c)
myrtos_add_module(fill modules/fill/fill.c)
myrtos_add_module(sleep modules/sleep/sleep.c)
myrtos_add_module(spin modules/spin/spin.c)
myrtos_add_module(nice modules/nice/nice.c)
myrtos_add_module(ps modules/ps/ps.c)
myrtos_add_module(memtest modules/memtest/memtest.c)
myrtos_add_module(pimpl modules/pimpl/pimpl.c modules/pimpl/counter.c)
myrtos_add_module(bootsel modules/bootsel/bootsel.c)
myrtos_add_data_module(termdesc modules/term_desc/term_desc.c)
myrtos_add_data_module(usbdesc modules/usb_desc/usb_desc.c)

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
            ${CMAKE_CURRENT_BINARY_DIR}/nice.mod
            ${CMAKE_CURRENT_BINARY_DIR}/ps.mod
            ${CMAKE_CURRENT_BINARY_DIR}/memtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/pimpl.mod
            ${CMAKE_CURRENT_BINARY_DIR}/bootsel.mod
            ${CMAKE_CURRENT_BINARY_DIR}/termdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbdesc.mod)
