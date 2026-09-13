# The Pico VS Code extension wrote the block below when the project was imported,
# and it is kept because it earns its place: pico-vscode.cmake sets PICO_SDK_PATH
# and, seeing RISCV in the toolchain name, PICO_PLATFORM -- on every configure,
# from a file that is in the repository. That is exactly what a stale cache
# holding a wrong SDK path could not survive, and one of those cost an afternoon.
#
# One line is edited despite what the banner says: toolchainVersion is chosen
# from MYRTOS_ARCH below rather than written in. The import set it to 15_2_Rel1
# once, which is arm-none-eabi, and for a RISC-V build that cannot compile a
# line. If an extension update ever writes it back, that is what to look for.

# Which machine. -DMYRTOS_ARCH=arm builds for the RP2350's Cortex-M33 cores
# instead of its Hazard3 ones.
#
# It has to be decided here, before anything else, because the SDK's toolchain
# and platform are settled at import and cannot be changed afterwards -- and
# because pico-vscode.cmake reads the toolchain's NAME to guess the platform.
# Everything further down that differs by machine follows from this one word.
set(MYRTOS_ARCH arm CACHE STRING "riscv (Hazard3) or arm (Cortex-M33)")
set_property(CACHE MYRTOS_ARCH PROPERTY STRINGS riscv arm)

if(MYRTOS_ARCH STREQUAL "riscv")
    set(myrtosToolchain RISCV_ZCB_RPI_2_2_0_3)
    set(myrtosPlatform  rp2350-riscv)
elseif(MYRTOS_ARCH STREQUAL "arm")
    set(myrtosToolchain 15_2_Rel1)
    set(myrtosPlatform  rp2350-arm-s)
else()
    message(FATAL_ERROR "MYRTOS_ARCH is '${MYRTOS_ARCH}'; it must be riscv or arm")
endif()

# What a module is compiled and linked with -- a code model, a reserved
# register, a linker that must not relax -- is in sdk/myrtos-sdk.cmake, together
# with myrtos_add_module itself. It used to be here, and a module could
# therefore only be built inside this tree. The include is further down, after
# pico_sdk_init, because building a module needs the toolchain; nothing between
# here and there uses any of it.

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# ====================================================================================

# NOTHING ABOVE THIS LINE MAY CONTAIN A VARIABLE, NOT EVEN IN A COMMENT.
#
# The extension reads that block as TEXT rather than running it, and hands the
# result to CMake as -DCMAKE_C_COMPILER on the command line -- where it beats
# anything this file works out. A dollar sign survives the trip and arrives as
# part of a path, so the compiler is looked for at a directory whose name still
# has the reference in it, and the error says the toolchain is not installed.
#
# This has now happened twice. The first time the toolchain version was set from
# a variable. The second time it was a COMMENT explaining the first time, which
# spelled the same path out to describe it -- and the parser does not know the
# difference between a line and a line about a line.
#
# So the version above names the Arm toolchain literally, and the other machine
# is selected here instead, where ordinary CMake rules apply. It has to be
# before project(), and it is.
if(NOT myrtosToolchain STREQUAL toolchainVersion)
    set(PICO_TOOLCHAIN_PATH ${USERHOME}/.pico-sdk/toolchain/${myrtosToolchain})
endif()

# picotoolVersion above is deliberately not sdkVersion: the SDK asks for the
# picotool it needs and 2.3.1 asks for 2.3.0 -- tools/CMakeLists.txt sets
# picotool_VERSION_REQUIRED -- so matching them would point at a directory that
# does not exist and send the build off to compile picotool from source.

# --- WHICH BOARD ----------------------------------------------------------
# The machine myrtos is being built for, as its own dimension. It was four
# things before: PICO_BOARD here, plus pin numbers in kernel/main.c,
# kernel/usbhost.c, kernel/video.c and kernel/pins.c. Four places that happened
# to agree about one board, which is fine until there are two.
#
# This selects the SDK's board (for its clocks, flash and PSRAM), and names a
# header the kernel reads for everything else -- see kernel/board.h.
#
# MYRTOS_ARCH and MYRTOS_VIDEO stay separate on purpose: a board can run either
# architecture and, where it has the pins for both, either video mode. Only the
# facts that FOLLOW from the board belong here.
set(MYRTOS_BOARD fruit-jam CACHE STRING "which board to build for")
set_property(CACHE MYRTOS_BOARD PROPERTY STRINGS fruit-jam ws43b)

if(MYRTOS_BOARD STREQUAL "fruit-jam")
    set(myrtosPicoBoard adafruit_fruit_jam)
    set(myrtosBoardHeader "<boards/fruit-jam.h>")
    set(myrtosBoardRadio ON)
    set(myrtosBoardVideo chargen)     # or framebuffer; both exist here
    set(myrtosBoardModules
        fat32lib sdlib                # the SD card
        leddesc i2cdesc audiodesc adcdesc gpiodesc
        termdesc usbdesc kbddesc condesc acmdesc
        espdesc ehdesc)               # the radio
elseif(MYRTOS_BOARD STREQUAL "ws43b")
    # The SDK has no header for this one, so we carry ours -- see
    # boards/pico/. PICO_BOARD_HEADER_DIRS is how the SDK is told where to look.
    set(myrtosPicoBoard waveshare_rp2350_touch_lcd_4_3b)
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/pico
        CACHE STRING "where to find board headers the SDK does not ship")
    set(myrtosBoardHeader "<boards/ws43b.h>")
    set(myrtosBoardRadio OFF)         # no ESP32-C6 on this board
    set(myrtosBoardVideo rgb)         # the ST7262 panel, no console on it yet
    # The USB console and the panel's own. Every other descriptor names pins
    # this board uses for something else -- the panel has twenty of them -- and
    # a driver that configures the wrong pin is worse than an absent one.
    #
    # condesc carries no pins at all: it names the console driver, which is in
    # the kernel, so it is the one descriptor that is the same on both boards.
    # acmdesc is the BleuIO on the soldered host socket: a CDC-ACM device, which
    # the hibouair module already knows how to talk to. No kbddesc -- the `con`
    # shell reads the host's key queue directly, so a keyboard would work on it
    # without a descriptor, and there is one socket.
    set(myrtosBoardModules usbdesc condesc acmdesc gt911 touchdesc touch)
else()
    message(FATAL_ERROR "MYRTOS_BOARD is '${MYRTOS_BOARD}'; there is a header for "
                        "each supported board in boards/, and no such file")
endif()
message(STATUS "Board: ${MYRTOS_BOARD}")

set(PICO_BOARD ${myrtosPicoBoard} CACHE STRING "Board type")

# Force the whole Pico SDK to the same machine MYRTOS_ARCH chose. The two must
# agree: the SDK's own assembly, its interrupt handling and its crt0 are all
# built from this.
set(PICO_PLATFORM ${myrtosPlatform} CACHE STRING "Pico platform")

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
# The machine's own half of the kernel. On RISC-V the trap vector is one file
# and the timer lives at the bottom of scheduler.c, where it reads the machine
# timer directly; on ARM the vector is its own file and SysTick is another,
# because neither is the same device.
if(MYRTOS_ARCH STREQUAL "riscv")
    set(MYRTOS_ARCH_KERNEL_SOURCES kernel/scheduler.S)
else()
    set(MYRTOS_ARCH_KERNEL_SOURCES kernel/arm/scheduler.S kernel/arm/timer.c kernel/arm/stack.c)
endif()

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
    kernel/pins.c
    kernel/sdlink.c
    kernel/fat32link.c
    kernel/moddir.c
    kernel/flashmod.c
    kernel/usbdev.c
    kernel/usb_descriptors.c
        kernel/usbmsc.c
    ${MYRTOS_ARCH_KERNEL_SOURCES}
    kernel/pio/pioprobe.c
    kernel/usbhost.c
    # video.c, console.c and chargen.c are added with the video mode below.
    kernel/net.c
    kernel/config.c
    kernel/clock.c
    kernel/fsserver.c
    kernel/wifilink.c
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
    # Ours too: the hub stops polling for ever after one failed transfer, which
    # is how an unplug goes unreported. See lib/tinyusb-patched/README.md.
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb-patched/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
    # Ours, and the only file of TinyUSB's that is. An unplugged CDC device
    # asserts in cdch_xfer_cb, and that assert halts the core when a debugger is
    # attached -- see lib/tinyusb-patched/README.md for the measurement and for
    # how to re-sync this after an SDK upgrade.
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinyusb-patched/class/cdc/cdc_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# Execution happens in RAM, but the image lives in flash and is copied there at
# boot. That is deliberate: the boot ROM reads PICOBIN_IMAGE_TYPE_EXE_CPU from
# the image and only then switches to RISC-V. With no_flash there is no image to
# switch on, so the chip starts in ARM and the debugger's reset-and-halt finds
# no RISC-V core.
pico_set_binary_type(os_kernel copy_to_ram)

pico_generate_pio_header(os_kernel ${CMAKE_CURRENT_SOURCE_DIR}/kernel/pio/probe.pio)

# The SD card driver is not built into the kernel any more. It is the sdlib
# module -- see the myrtos_add_module call for it further down, and the note at
# the top of kernel/sdlink.c.

# Link against the Pico standard libraries and enable PSRAM support.
# pico_standard_link provides only linker scripts and link flags. The startup
# code crt0, which defines _entry_point and calls main, comes with pico_runtime.
# hardware_uart and hardware_gpio are for the diagnostic port.
target_link_libraries(os_kernel pico_runtime pico_multicore pico_unique_id hardware_uart hardware_gpio hardware_spi pico_time
    pico_bootrom hardware_pio hardware_dma hardware_clocks pico_stdlib hardware_psram
    hardware_i2c hardware_adc tinyusb_device)

# TinyUSB looks for tusb_config.h on the include path.
# What a kernel critical section masks, and it is BASEPRI by default.
#
# 0x80 is the threshold, and it is chosen rather than found: every peripheral
# the SDK sets up takes 0x80 and the scheduler sits at 0xF0, so masking down to
# 0x80 masks everything that exists while leaving 0x00 to 0x70 entirely free
# for a driver that needs to outrank the kernel. Measured against a deliberate
# 500 us critical section, a handler at 0x40 sees a worst-case latency of 127
# microseconds where PRIMASK gave it 555 -- see `crit` and `adc -i`.
#
# Set it empty to go back to PRIMASK, which masks everything and is what this
# system did before. Arm only: RISC-V has no equivalent and takes the hammer
# either way, which kernel/critical.h decides for itself.
#
# THE RULE THAT COMES WITH IT is in the README under "What a handler above the
# kernel may not do", and it is not optional.
set(MYRTOS_BASEPRI "0x80" CACHE STRING "priority value to mask down to; empty for PRIMASK")
if(MYRTOS_BASEPRI)
    target_compile_definitions(os_kernel PRIVATE MYRTOS_CRITICAL_BASEPRI=${MYRTOS_BASEPRI})
endif()

# How the picture is stored, which is the largest memory decision in the system.
#
# `framebuffer` keeps 640x480 bytes of RGB332 in SRAM -- 307200 of a 512 kB
# machine -- and lets anything draw anywhere. `chargen` keeps 80x30 character
# cells instead, 4800 bytes, and builds each scanline as the display asks for
# it, the way a 6845 or a TMS9918 did before RAM was cheap enough to hold a
# picture. It costs the ability to draw pixels and buys back a quarter of a
# megabyte.
#
# The console works either way. Nothing else does: a framebuffer is a bitmap
# and character cells are not, so whatever wants to draw must wait for the
# scanline renderer that will sit beside the character generator.
# Four modes, and they answer two separate questions: is there a display driver,
# and is there a text console on it. `rgb` is a display with no console yet --
# the panel scanning out while the character generator that will feed it is
# still to come -- and `none` is neither.
set(MYRTOS_VIDEO "${myrtosBoardVideo}" CACHE STRING "framebuffer, chargen, rgb, or none")
set_property(CACHE MYRTOS_VIDEO PROPERTY STRINGS framebuffer chargen rgb none)

set(myrtosHasVideo 1)
set(myrtosHasConsole 1)
if(MYRTOS_VIDEO STREQUAL "chargen")
    target_compile_definitions(os_kernel PRIVATE MYRTOS_VIDEO_CHARGEN=1)
elseif(MYRTOS_VIDEO STREQUAL "rgb")
    # A parallel RGB panel with the character generator on it. MYRTOS_VIDEO_RGB
    # picks the geometry -- see chargen.h -- and MYRTOS_VIDEO_CHARGEN is what
    # tells console.c to draw in cells, and what puts font8x16 in SRAM where the
    # renderer reads it from an interrupt.
    target_compile_definitions(os_kernel PRIVATE MYRTOS_VIDEO_CHARGEN=1 MYRTOS_VIDEO_RGB=1)
elseif(MYRTOS_VIDEO STREQUAL "none")
    set(myrtosHasVideo 0)
    set(myrtosHasConsole 0)
elseif(NOT MYRTOS_VIDEO STREQUAL "framebuffer")
    message(FATAL_ERROR "MYRTOS_VIDEO is '${MYRTOS_VIDEO}'; it must be framebuffer, chargen, rgb or none")
endif()
target_compile_definitions(os_kernel PRIVATE
    MYRTOS_HAS_VIDEO=${myrtosHasVideo} MYRTOS_HAS_CONSOLE=${myrtosHasConsole})
message(STATUS "Video: ${MYRTOS_VIDEO}")

# Added here rather than in the source list above, because which of them apply
# is the question this block answers. chargen.c is built in both real modes: the
# framebuffer console does not use it, and the linker drops what nothing calls.
# The console's API is called from main.c, syscalls.c and io.c, so it cannot
# simply vanish where there is none. console_none.c answers it honestly instead
# -- see the note at the top of that file.
if(NOT myrtosHasConsole)
    target_sources(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel/console_none.c)
endif()

if(MYRTOS_VIDEO STREQUAL "rgb")
    # The panel's own driver instead of video.c, and the same character
    # generator and console as the other display. kernel/pio/rgb.pio holds the
    # timing.
    target_sources(os_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/videorgb.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/console.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/chargen.c)
    pico_generate_pio_header(os_kernel ${CMAKE_CURRENT_SOURCE_DIR}/kernel/pio/rgb.pio)
elseif(myrtosHasVideo)
    target_sources(os_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/console.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/chargen.c)
endif()

# The module pool: a fixed array in SRAM, which is where modules are loaded and
# where a driver's DMA-reachable buffers come from. Freeing space at link time
# does not enlarge it -- 9 Sep 2026 the character generator handed back 300 kB
# and `free` went on reporting 10260 bytes until this number moved.
#
# It has to follow MYRTOS_VIDEO, because a framebuffer build has 28 kB of SRAM
# to spare and a chargen build has 300, so "auto" resolves against the mode
# rather than being cached as a number that stops being right when the mode
# changes. Give it a number in kB to override.
set(MYRTOS_HEAP_KB "auto" CACHE STRING "module pool in kB, or auto to follow MYRTOS_VIDEO")
if(MYRTOS_HEAP_KB STREQUAL "auto")
    if(NOT MYRTOS_VIDEO STREQUAL "chargen")
        # 36 rather than 40 since 12 Sep 2026, and the four kilobytes bought
        # ninety-eight bytes. The riscv framebuffer build sat that far below a
        # 4 kB alignment boundary: the kernel grew by k_i2c_instance and one
        # pointer in the API table, and `end` moved a whole page, taking the C
        # heap from 4916 bytes to 820.
        #
        # Worth writing down because the next change will do it again. The
        # number to watch is not how much was added but how close `end` is to
        # the next page -- and check_heap is what notices.
        set(myrtosHeapKB 36)
    else()
        # 192 on both. It started at 256 on Arm, but every static the kernel
        # gains comes out of the C heap between end and the stack rather than
        # out of this pool, and the scanline buffers, the scrollback ring, the
        # console trace took it from 79 kB to 11. This
        # project has already had a boot crash from a C heap that reached zero
        # and made the SDK panic before init. The pool is still nearly five
        # times what it was before the framebuffer went, and `free` has never
        # shown it under strain.
        # 160, down from 256 over the day, because every kernel static comes
        # out of the C heap and not out of this pool: the scanline buffers, the
        # scrollback and now lwIP. At 184 the C heap was 6644
        # bytes, and a C heap that reaches zero has already crashed this board
        # once. The pool is still four times what it was this morning.
        # 128. It has come down all day as the kernel's own statics grew --
        # scanline buffers, scrollback, lwIP, and mDNS last of
        # all, which cost 22 kB and took the Arm C heap to 8912 bytes while
        # RISC-V simply would not link. Every one of those comes out of the C
        # heap and not out of this pool, and a C heap that reaches zero has
        # crashed this board before. The pool is still three times what it was
        # this morning and `free` has never been near it.
        #
        # 120 rather than 128 since 11 Sep 2026, in two steps of four. The wall
        # clock and its SNTP client took the first; kernel/font8x16.c moving
        # into SRAM took the second, which is the fix for the board stopping
        # under its own video interrupt -- see the note in that file.
        #
        # Both came out of here because this pool has room to spare and the C
        # heap has none, which is the trade the paragraph above describes. Only
        # the chargen builds pay: a framebuffer build keeps its font in flash
        # because nothing reads it from an interrupt there.
        set(myrtosHeapKB 120)
    endif()
else()
    set(myrtosHeapKB ${MYRTOS_HEAP_KB})
endif()
# A plain number, not "N*1024": a bare product substituted into an expression
# binds wrong, and `(x) % DMESG_SIZE` quietly became `(x) % 16 * 1024`.
math(EXPR myrtosHeapBytes "${myrtosHeapKB} * 1024")
target_compile_definitions(os_kernel PRIVATE MYRTOS_HEAP_SIZE=${myrtosHeapBytes})
message(STATUS "Module pool: ${myrtosHeapKB} kB (MYRTOS_VIDEO=${MYRTOS_VIDEO})")

# The kernel log ring, on the same terms and for the same reason: 2 kB wrapped
# before the shell came up, so the boot banner was gone before anyone could read
# it. Whole boot plus room for what happens afterwards.
set(MYRTOS_DMESG_KB "auto" CACHE STRING "kernel log ring in kB, or auto to follow MYRTOS_VIDEO")
if(MYRTOS_DMESG_KB STREQUAL "auto")
    if(MYRTOS_VIDEO STREQUAL "chargen")
        set(myrtosDmesgKB 16)
    else()
        set(myrtosDmesgKB 2)
    endif()
else()
    set(myrtosDmesgKB ${MYRTOS_DMESG_KB})
endif()
math(EXPR myrtosDmesgBytes "${myrtosDmesgKB} * 1024")
target_compile_definitions(os_kernel PRIVATE DMESG_SIZE=${myrtosDmesgBytes})
message(STATUS "Kernel log: ${myrtosDmesgKB} kB")

# lwIP, and it does not fit beside a framebuffer. That is not a limitation of
# the build system, it is 307200 bytes: with the bitmap in SRAM the link
# overflows RAM by about 40 kB however the pools are trimmed. So "auto" follows
# MYRTOS_VIDEO the way the other sizes do, and the machine that wants a network
# is the one that gave up its framebuffer for it.
set(MYRTOS_LWIP "auto" CACHE STRING "on, off, or auto to follow MYRTOS_VIDEO")
if(MYRTOS_LWIP STREQUAL "auto")
    if(MYRTOS_VIDEO STREQUAL "chargen")
        set(myrtosLwip ON)
    else()
        set(myrtosLwip OFF)
    endif()
else()
    set(myrtosLwip ${MYRTOS_LWIP})
endif()
message(STATUS "lwIP: ${myrtosLwip}")
if(myrtosLwip)
    target_sources(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel/lwipnet.c
                                     ${CMAKE_CURRENT_SOURCE_DIR}/kernel/lwipping.c
                                     ${CMAKE_CURRENT_SOURCE_DIR}/kernel/lwipmdns.c
                                     ${CMAKE_CURRENT_SOURCE_DIR}/kernel/lwipsntp.c
                                     ${CMAKE_CURRENT_SOURCE_DIR}/kernel/lwipsock.c)
    # The WiFi netif only where there is a radio behind it.
    if(myrtosBoardRadio)
        target_sources(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel/ehnet.c)
    endif()
    target_link_libraries(os_kernel pico_lwip pico_lwip_nosys pico_lwip_mdns)
    target_compile_definitions(os_kernel PRIVATE MYRTOS_LWIP=1)
endif()

# The repository root is on the path so that <boards/...> resolves; the kernel
# asks for its board through kernel/board.h and never by name.
target_compile_definitions(os_kernel PRIVATE MYRTOS_BOARD_HEADER=${myrtosBoardHeader})

target_include_directories(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/Pico-PIO-USB/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/include
    # Where our copy of cdc_host.c finds the header that used to sit beside it.
    # A quoted include falls back to the include path, so the copy needs no edit
    # of its own to compile from somewhere else.
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/cdc
    ${PICO_SDK_PATH}/lib/tinyusb/src/host)

# Enable extra warnings and optimisation for the RTOS
target_compile_options(os_kernel PRIVATE -Wall -O2)

# Two exceptions, for size rather than speed. Date formatting and an SNTP query
# happen once a second at the very most and are nowhere near any deadline, while
# SRAM is the thing this machine is actually short of -- the kernel is linked
# copy_to_ram, so every byte of code is a byte of RAM. -O3 spent 868 bytes on
# myrtos_clock_stamp alone.
set_source_files_properties(kernel/clock.c kernel/lwipsntp.c
    PROPERTIES COMPILE_OPTIONS "-Os")

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
# myrtos_add_module, and the per-machine flags a module is built with. Extracted
# so that a repository which is not this one can build a module against the same
# knowledge -- and extracted rather than copied, so there is no second version
# of it to go stale. Every module below is built by the SDK's function.
include(${CMAKE_CURRENT_SOURCE_DIR}/sdk/myrtos-sdk.cmake)

# --- The second toolchain -------------------------------------------------
# Clang and lld, beside GCC rather than instead of it, chosen per module.
#
# There was one reason to want it and it was C++ with virtual functions: a
# vtable is a table of pointers, which a module could not contain until the
# loader could relocate. Clang's -fexperimental-relative-c++-abi-vtables stored
# the distance from each slot to its function instead of the address, and was
# never quite enough -- an object still holds an absolute pointer to its vtable,
# so a static one was refused anyway. The loader settles it: a plain vtable is
# four addresses it fixes at load, and the flag is gone. vtdemo is the proof.
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
# The probe compiles for whichever machine this build is for, with the same
# triple and the same flags the modules will use -- so it answers the question
# actually being asked rather than a fixed one. It used to offer riscv32 and
# nothing else, which is what made clang modules riscv-only.
if(MYRTOS_CLANG AND MYRTOS_CLANGXX AND MYRTOS_LLD)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.c "int probe(void){return 0;}\n")
    execute_process(
        COMMAND ${MYRTOS_CLANG} --target=${MYRTOS_LLVM_TRIPLE} ${MYRTOS_TARGET_ARCH}
                ${MYRTOS_LLVM_CFLAGS} ${MYRTOS_MODULE_CFLAGS}
                -ffreestanding -nostdlib -c ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.c
                -o ${CMAKE_CURRENT_BINARY_DIR}/clang_probe.o
        RESULT_VARIABLE probe_rc OUTPUT_QUIET ERROR_VARIABLE probe_err)
    if(probe_rc EQUAL 0)
        set(MYRTOS_CLANG_OK TRUE)
        message(STATUS "Clang module toolchain: ${MYRTOS_CLANG} + ${MYRTOS_LLD} (${MYRTOS_LLVM_TRIPLE})")
    else()
        message(STATUS "Clang found but cannot build for this target; modules fall back to gcc")
        message(STATUS "  ${probe_err}")
    endif()
else()
    message(STATUS "No clang/lld found; clang modules fall back to gcc")
endif()

# -std=gnu23 because clang defaults to a C where bool is not a keyword.
#
# -fno-jump-tables is gone. It was here because a large switch becomes a table
# of absolute addresses -- sh came out with 97 R_RISCV_32 with it removed --
# and absolute addresses were once refused outright. The loader fixes them now,
# so the compiler may generate the code it thinks is best and pay four bytes a
# case for it on the card.
set(MYRTOS_CLANG_C_FLAGS   -std=gnu23)
set(MYRTOS_CLANG_CXX_FLAGS -fno-exceptions -fno-rtti -fno-threadsafe-statics
                           -fno-use-cxa-atexit)

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
        set(single_link "")   # relocated like every other module
        set(single_compile "-fno-zero-initialized-in-bss")
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()

    set(objdir ${CMAKE_CURRENT_BINARY_DIR}/${name}_clang)
    file(MAKE_DIRECTORY ${objdir})
    # The architecture's own sources come along, exactly as they do in
    # myrtos_add_module: on ARM that is common/arm/tp.S, the two instructions
    # that answer __aeabi_read_tp. A clang module without it links but cannot
    # read a thread-local.
    set(objs "")
    foreach(src ${source} ${ARGN} ${MYRTOS_MODULE_ARCH_SOURCES})
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
        # An absolute path is left alone; MYRTOS_MODULE_ARCH_SOURCES gives one.
        set(srcpath ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        if(IS_ABSOLUTE ${src})
            set(srcpath ${src})
        endif()
        add_custom_command(
            OUTPUT ${obj}
            COMMAND ${cc} --target=${MYRTOS_LLVM_TRIPLE} ${MYRTOS_TARGET_ARCH}
                    -fno-pic ${MYRTOS_MODULE_CFLAGS} ${MYRTOS_LLVM_CFLAGS}
                    -fno-common -ffreestanding -nostdlib -O2
                    ${lang_flags} ${single_compile}
                    -c ${srcpath} -o ${obj}
            DEPENDS ${srcpath}
            COMMENT "Compiling ${src} with clang")
        list(APPEND objs ${obj})
    endforeach()

    # lld needs no --no-warn-rwx-segments: it does not warn about them. -N and
    # -e 0 mean what they mean for GNU ld -- see the note in myrtos_add_module.
    set(elf ${objdir}/${name}.elf)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${MYRTOS_LLD} -m ${MYRTOS_LLVM_EMULATION} -N -e 0 --emit-relocs ${single_link} -o ${elf} ${objs}
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
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building module ${name}.mod (clang)")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in MYRTOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

# --- Rust, natively --------------------------------------------------------
# Not a cross-compiler of its own: rustc is LLVM, so it is the same backend the
# clang rule already uses, and it wants the same three things -- no PIC,
# literal pools instead of movw/movt, and r9 left alone for the thread pointer.
#
# The target is the machine's, taken from MYRTOS_LLVM_TRIPLE's neighbourhood
# rather than written in, so this cannot quietly become ARM-only the way the
# clang rule did. rustup has to have the target installed; if it does not, the
# module is skipped and said out loud, like every other optional toolchain here.
find_program(MYRTOS_RUSTC rustc)
set(MYRTOS_RUST_OK FALSE)
if(MYRTOS_ARCH STREQUAL "riscv")
    set(MYRTOS_RUST_TARGET riscv32imac-unknown-none-elf)
else()
    # hf rather than the soft-float target because that is the one installed
    # here. Nothing crosses the module boundary as a float -- every syscall
    # argument is a uint32 -- so the difference does not reach the ABI.
    set(MYRTOS_RUST_TARGET thumbv8m.main-none-eabihf)
endif()
if(MYRTOS_RUSTC AND MYRTOS_LLD)
    execute_process(COMMAND ${MYRTOS_RUSTC} --print target-list
                    OUTPUT_VARIABLE rust_targets RESULT_VARIABLE rustc_rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND rustup target list --installed
                    OUTPUT_VARIABLE rust_installed ERROR_QUIET)
    if(rustc_rc EQUAL 0 AND rust_installed MATCHES "${MYRTOS_RUST_TARGET}")
        set(MYRTOS_RUST_OK TRUE)
        message(STATUS "Rust module toolchain: ${MYRTOS_RUSTC} (${MYRTOS_RUST_TARGET})")
    else()
        message(STATUS "Rust target ${MYRTOS_RUST_TARGET} is not installed; Rust modules are skipped")
    endif()
else()
    message(STATUS "No rustc; Rust modules are skipped")
endif()

# Same downstream as every other module: the same checker, the same objcopy,
# the same packager, the same .mod.
function(myrtos_add_rust_module name source)
    if(NOT MYRTOS_RUST_OK)
        return()
    endif()

    set(rt_flag "")
    if("RT" IN_LIST ARGN)
        set(rt_flag "--rt")
        list(REMOVE_ITEM ARGN "RT")
    endif()

    set(objdir ${CMAKE_CURRENT_BINARY_DIR}/${name}_rust)
    file(MAKE_DIRECTORY ${objdir})
    set(obj ${objdir}/${name}.o)
    set(elf ${objdir}/${name}.elf)

    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${MYRTOS_RUSTC} --target ${MYRTOS_RUST_TARGET}
                -C opt-level=2 -C panic=abort -C relocation-model=static
                -C target-feature=+no-movt,+reserve-r9
                --emit obj -o ${obj} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        COMMENT "Compiling ${source} with rustc")

    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${MYRTOS_LLD} -m ${MYRTOS_LLVM_EMULATION} -N -e 0 --emit-relocs -o ${elf} ${obj}
        DEPENDS ${obj}
        COMMENT "Linking ${name} with lld")

    add_custom_command(
        OUTPUT ${objdir}/${name}.checked
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${CMAKE_READELF} ${obj} ${elf}
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
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}" ${elf} ${CMAKE_NM} module_main ${rt_flag}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building module ${name}.mod (rustc)")

    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

# --- The third toolchain --------------------------------------------------
# LDC, the LLVM D compiler, beside the other two.
#
# D is here for one property: it puts every module-level and static variable in
# thread-local storage unless told otherwise. That is not a convenience, it is
# exactly what a shareable module needs, and in D it is the default rather than
# something every declaration has to remember. The C modules get there with
# __thread on each variable and a checker to catch the ones that forgot.
#
# Everything downstream is the same. A D module goes through the same
# check_module.py, the same objcopy and the same make_module.py, and lands as
# the same ${name}.mod.
find_program(MYRTOS_LDC ldc2 HINTS /opt/homebrew/bin /usr/local/bin)

# -betterC is what makes D usable here: no druntime, no garbage collector, no
# exceptions, no ModuleInfo. What survives is the part worth having -- slices,
# templates, compile-time evaluation, scope guards, and @nogc/nothrow as
# contracts the compiler checks.
#
# --fthread-model=local-exec is the flag the whole thing turns on. Without it
# LDC emits R_RISCV_TLS_GOT_HI20 for a thread-local: the general-dynamic model
# reaches its variable through a GOT, and a GOT entry is an absolute address the
# linker writes in, which is the one thing a module may not contain. Local exec
# gives TPREL_HI20/LO12/ADD -- the same relocations __thread produces in C, and
# the ones check_module.py already allows. Measured both ways before this went
# in: the general-dynamic build is refused as not position independent and the
# local-exec build passes.
#
# --relocation-model=static is -fno-pic and --code-model=medium is -mcmodel=medany.
#
# --code-model is a RISC-V idea and has no ARM counterpart, so it goes with the
# triple rather than with the flags every machine shares. --fthread-model does
# carry across: local-exec gives R_ARM_TLS_LE32, which is an offset from the
# thread pointer and is on check_module.py's allowed list for the same reason
# TPREL is.
if(MYRTOS_ARCH STREQUAL "riscv")
    set(MYRTOS_LDC_ARCH -mtriple=riscv32-unknown-elf -mattr=+m,+a,+c,+zba,+zbb,+zbs
                        --code-model=medium)
else()
    # +no-movt because LLVM otherwise materialises an absolute address as a
    # movw/movt pair, which is ARM's answer to RISC-V's lui/addi and needs the
    # same kind of two-instruction relocation -- R_ARM_THM_MOVW_ABS_NC and
    # R_ARM_THM_MOVT_ABS, neither of which the loader knows. Without it the
    # address goes in a literal pool instead: one plain word in .text, one
    # R_ARM_ABS32, and the loader has handled that since the day it was
    # written. It is also what gcc does for every C module here.
    set(MYRTOS_LDC_ARCH -mtriple=thumbv8m.main-none-eabi -mcpu=cortex-m33
                        -mattr=+no-movt)
endif()
set(MYRTOS_LDC_FLAGS -betterC -Oz --relocation-model=static
                     --fthread-model=local-exec)

set(MYRTOS_LDC_OK FALSE)
if(MYRTOS_LDC)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ldc_probe.d
         "module ldc_probe;\nextern(C) int probe() @nogc nothrow { return 0; }\n")
    execute_process(
        COMMAND ${MYRTOS_LDC} -c ${MYRTOS_LDC_FLAGS} ${MYRTOS_LDC_ARCH}
                ${CMAKE_CURRENT_BINARY_DIR}/ldc_probe.d
                -of=${CMAKE_CURRENT_BINARY_DIR}/ldc_probe.o
        RESULT_VARIABLE dprobe_rc OUTPUT_QUIET ERROR_VARIABLE dprobe_err)
    if(dprobe_rc EQUAL 0)
        set(MYRTOS_LDC_OK TRUE)
        message(STATUS "D module toolchain: ${MYRTOS_LDC}")
    else()
        message(STATUS "ldc2 found but cannot build for this target; D modules skipped")
        message(STATUS "  ${dprobe_err}")
    endif()
else()
    message(STATUS "No ldc2 found; D modules skipped")
endif()

# Same signature and same RT / SINGLE options as myrtos_add_module. Unlike the
# clang one this cannot fall back to another compiler -- there is no gcc that
# reads D -- so a machine without ldc2 gets no module rather than a broken
# build, and says so at configure time.
function(myrtos_add_d_module name source)
    if(NOT MYRTOS_LDC_OK)
        message(STATUS "Skipping D module ${name}: no usable ldc2")
        return()
    endif()

    set(rt_flag "")
    if("RT" IN_LIST ARGN)
        set(rt_flag "--rt")
        list(REMOVE_ITEM ARGN "RT")
    endif()
    set(single_flag "")
    set(single_link "")
    if("SINGLE" IN_LIST ARGN)
        set(single_flag "--single")
        set(single_link "")   # relocated like every other module
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()

    set(objdir ${CMAKE_CURRENT_BINARY_DIR}/${name}_d)
    file(MAKE_DIRECTORY ${objdir})

    # Every D source of the module in one ldc2 invocation, with common/myrtos.d
    # among them, and --singleobj so it comes out as one object.
    #
    # Importing a D module is not linking against it: an import brings in the
    # declarations, and the definitions only exist if that file was compiled
    # too. common/myrtos.d also carries memset, memcpy and __assert, which LDC
    # emits calls to and a hosted target would take from a C library.
    set(dsrcs "")
    # The architecture's own sources come along, exactly as they do in
    # myrtos_add_module: on ARM that is common/arm/tp.S, the two instructions
    # that answer __aeabi_read_tp. A clang module without it links but cannot
    # read a thread-local.
    set(objs "")
    foreach(src ${source} ${ARGN} ${MYRTOS_MODULE_ARCH_SOURCES})
        get_filename_component(ext ${src} EXT)
        if(ext STREQUAL ".d")
            list(APPEND dsrcs ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()
    endforeach()
    if(dsrcs)
        set(dobj ${objdir}/${name}_d.o)
        add_custom_command(
            OUTPUT ${dobj}
            COMMAND ${MYRTOS_LDC} -c --singleobj ${MYRTOS_LDC_FLAGS} ${MYRTOS_LDC_ARCH}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/common
                    ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos.d ${dsrcs} -of=${dobj}
            DEPENDS ${dsrcs} ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos.d
            COMMENT "Compiling ${name} with ldc2")
        list(APPEND objs ${dobj})
    endif()

    foreach(src ${source} ${ARGN})
        get_filename_component(base ${src} NAME_WE)
        get_filename_component(ext ${src} EXT)
        if(ext STREQUAL ".d")
            continue()          # already in the single ldc2 object above
        endif()
        set(obj ${objdir}/${base}.o)
        if(TRUE)
        else()
            # A C file alongside, on the same terms as any other module's.
            add_custom_command(
                OUTPUT ${obj}
                COMMAND ${CMAKE_C_COMPILER} ${MYRTOS_TARGET_ARCH}
                        -fno-pic -mcmodel=medany -fno-common -ffreestanding
                        -nostdlib -O2 -c ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
                COMMENT "Compiling ${src} for D module ${name}")
        endif()
        list(APPEND objs ${obj})
    endforeach()

    # GNU ld through the compiler driver, exactly as myrtos_add_module links.
    # -N so the writable sections are not page-aligned: without it .tbss lands a
    # page past the code and objcopy pads the gap, which is four kilobytes of
    # zeroes in every module with a thread-local -- and in D that is every
    # module with a variable.
    set(elf ${objdir}/${name}.elf)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${CMAKE_C_COMPILER} ${MYRTOS_TARGET_ARCH} -nostdlib
                ${MYRTOS_MODULE_LDFLAGS} -Wl,-N -Wl,-e,0 -Wl,--no-warn-rwx-segments
                -Wl,--emit-relocs ${single_link} ${objs}
                ${MYRTOS_MODULE_ARCH_SOURCES} -o ${elf}
        DEPENDS ${objs}
        COMMENT "Linking ${name} (D)")

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
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building module ${name}.mod (D)")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in MYRTOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

# --- ZIG MODULES ----------------------------------------------------------
# Zig reaches this target with nothing installed but zig itself, and needs no
# flag to get thread-local storage right: `threadlocal var` on a freestanding
# riscv32 gives TPREL_HI20/ADD/LO12_I, which is what a shareable module needs
# and what check_module.py already allows. LDC needs --fthread-model=local-exec
# to say the same thing.
#
# What it cannot do is make EVERY variable thread-local. A plain module-scope
# `var` lands in .data and is shared between every process running the module,
# and the checker refuses it -- the same discipline C has with __thread, with
# better habits, since `var` at module scope stands out in Zig where a static
# does not in C. D remains the only language here whose default is right.
#
# ReleaseSmall rather than ReleaseFast: it strips, and it keeps the panic
# handler from dragging in formatting machinery that has nowhere to print.
find_program(MYRTOS_ZIG zig HINTS /opt/homebrew/bin /usr/local/bin)

# -mcmodel=medium is -mcmodel=medany: without it Zig addresses a global with
# R_RISCV_HI20/LO12_I, an absolute address, and check_module.py refuses the
# module. It is the same flag every C module here is built with, and the first
# Zig build was rejected for exactly the reason the checker exists.
#
# ARM has no such flag and needs none: Thumb-2 reaches a global through a
# literal pool, so the absolute address is a plain word in .text that the
# loader relocates. It goes with the target rather than with the shared flags,
# because passing it there would simply be rejected.
if(MYRTOS_ARCH STREQUAL "riscv")
    set(MYRTOS_ZIG_ARCH -target riscv32-freestanding
                        -mcpu=generic_rv32+m+a+c+zba+zbb+zbs
                        -mcmodel=medium)
else()
    # -fno-unwind-tables because LLVM emits an .ARM.exidx entry for every
    # function on this target, and an entry saying a function cannot be unwound
    # still names a personality routine that does not exist here. On this side
    # only, so RISC-V modules come out byte for byte as they did.
    set(MYRTOS_ZIG_ARCH -target thumb-freestanding-eabi -mcpu=cortex_m33
                        -fno-unwind-tables)
endif()
set(MYRTOS_ZIG_FLAGS -OReleaseSmall -fno-PIC -fno-PIE
                     -fno-stack-check -fno-stack-protector)

set(MYRTOS_ZIG_OK FALSE)
if(MYRTOS_ZIG)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/zig_probe.zig
         "export fn probe() u32 { return 0; }\n")
    execute_process(
        COMMAND ${MYRTOS_ZIG} build-obj ${MYRTOS_ZIG_ARCH} ${MYRTOS_ZIG_FLAGS}
                -femit-bin=${CMAKE_CURRENT_BINARY_DIR}/zig_probe.o
                ${CMAKE_CURRENT_BINARY_DIR}/zig_probe.zig
        RESULT_VARIABLE zprobe_rc OUTPUT_QUIET ERROR_VARIABLE zprobe_err)
    if(zprobe_rc EQUAL 0)
        set(MYRTOS_ZIG_OK TRUE)
        message(STATUS "Zig module toolchain: ${MYRTOS_ZIG}")
    else()
        message(STATUS "zig found but cannot build for this target; Zig modules skipped")
        message(STATUS "  ${zprobe_err}")
    endif()
else()
    message(STATUS "No zig found; Zig modules skipped")
endif()

# Same signature and the same RT / SINGLE options as the others.
function(myrtos_add_zig_module name source)
    if(NOT MYRTOS_ZIG_OK)
        message(STATUS "Skipping Zig module ${name}: no usable zig")
        return()
    endif()

    set(rt_flag "")
    if("RT" IN_LIST ARGN)
        set(rt_flag "--rt")
        list(REMOVE_ITEM ARGN "RT")
    endif()
    set(single_flag "")
    set(single_link "")
    if("SINGLE" IN_LIST ARGN)
        set(single_flag "--single")
        set(single_link "")   # relocated like every other module
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()

    set(objdir ${CMAKE_CURRENT_BINARY_DIR}/${name}_zig)
    file(MAKE_DIRECTORY ${objdir})

    # common/myrtos.zig is reached as the module "myrtos", so a source says
    # @import("myrtos") and not a path -- the same shape D's `import myrtos`
    # has. One zig invocation produces one object; Zig follows the imports.
    set(zobj ${objdir}/${name}.o)
    add_custom_command(
        OUTPUT ${zobj}
        COMMAND ${MYRTOS_ZIG} build-obj ${MYRTOS_ZIG_ARCH} ${MYRTOS_ZIG_FLAGS}
                --dep myrtos
                -Mroot=${CMAKE_CURRENT_SOURCE_DIR}/${source}
                -Mmyrtos=${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos.zig
                -femit-bin=${zobj}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos.zig
        COMMENT "Compiling ${name} with zig")

    # -N so the writable sections are not page-aligned: without it .tbss lands a
    # page past the code and objcopy pads four kilobytes of zeroes into every
    # module that has a thread-local.
    set(elf ${objdir}/${name}.elf)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${CMAKE_C_COMPILER} ${MYRTOS_TARGET_ARCH} -nostdlib
                ${MYRTOS_MODULE_LDFLAGS} -Wl,-N -Wl,-e,0 -Wl,--no-warn-rwx-segments
                -Wl,--emit-relocs ${single_link} ${zobj}
                ${MYRTOS_MODULE_ARCH_SOURCES} -o ${elf}
        DEPENDS ${zobj}
        COMMENT "Linking ${name} (Zig)")

    add_custom_command(
        OUTPUT ${objdir}/${name}.checked
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${single_flag} ${CMAKE_READELF} ${zobj} ${elf}
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
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building module ${name}.mod (Zig)")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in MYRTOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

# A data module has no entry point: it IS its bytes.
function(myrtos_add_data_module name source)
    add_executable(${name}_app ${source})
    target_compile_options(${name}_app PRIVATE
        -fno-pic ${MYRTOS_MODULE_CFLAGS} -fno-common -ffreestanding -nostdlib -O2)
    target_link_options(${name}_app PRIVATE -nostdlib ${MYRTOS_MODULE_LDFLAGS} -Wl,-e,0
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
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building data module ${name}.mod")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in MYRTOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

myrtos_add_module(counter modules/counter/counter.c)
myrtos_add_clang_module(cxxdemo modules/cxxdemo/cxxdemo.cpp)
myrtos_add_clang_module(vtdemo modules/vtdemo/vtdemo.cpp)
myrtos_add_d_module(dhello modules/dhello/dhello.d)
myrtos_add_module(armtest modules/armtest/armtest.c)
myrtos_add_module(usbdisk modules/usbdisk/usbdisk.c)
myrtos_add_module(reboot modules/reboot/reboot.c)
myrtos_add_module(lsmod modules/lsmod/lsmod.c)
myrtos_add_module(free modules/freemem/free.c)
myrtos_add_module(usbstat modules/usbstat/usbstat.c)
myrtos_add_module(vidstat modules/vidstat/vidstat.c)
myrtos_add_module(pecho modules/pecho/pecho.c)
myrtos_add_zig_module(zhello modules/zhello/zhello.zig)
myrtos_add_module(echo modules/echo/echo.c)
myrtos_add_module(usbecho modules/usbecho/usbecho.c)
# Not RT, and it never should have been. A real-time module's memory comes out
# of the SRAM pool, and two shells at four kilobytes each -- plus the one the
# startup script needs -- were most of that pool. On 4 Sep 2026 the pool ran out
# and /sd/startup could not be given a shell to run it in.
#
# A shell has no timing constraint worth the name: it is blocked on a read
# almost all of the time, and when it runs it runs briefly. Its stack belongs in
# PSRAM with ls and cat and everything else that is merely a program. spinrt and
# keys stay RT, because measuring a scheduler and reading raw keystrokes are
# what the flag is for.
myrtos_add_module(sh modules/sh/sh.c)
myrtos_add_module(ls modules/ls/ls.c)
myrtos_add_module(cat modules/cat/cat.c)
myrtos_add_module(head modules/head/head.c)
myrtos_add_module(more modules/more/more.c)
myrtos_add_module(tlsftest modules/tlsftest/tlsftest.c)
myrtos_add_module(libctest modules/libctest/libctest.c)

# The same ground covered by the other library: qsort, bsearch, strdup and a
# real fopen, none of which the header-only subset has.
myrtos_add_module(newlibtest modules/newlibtest/newlibtest.c NEWLIB)

# The first library module: code the kernel calls rather than runs. It was
# kernel/wifi.c until 6 Sep 2026 -- see modules/wifilib/wifilib.c.
myrtos_add_module(httpd modules/httpd/httpd.c)
myrtos_add_module(mv modules/mv/mv.c)
myrtos_add_module(leds modules/leds/leds.c)
myrtos_add_module(sockstat modules/sockstat/sockstat.c)
myrtos_add_rust_module(rhello modules/rhello/rhello.rs)
# --- THE NINA RADIO, RETIRED 10 SEPTEMBER 2026 ------------------------------
#
# The ESP32-C6 ran NINA firmware until that day and carried its own TCP/IP
# stack: the host asked it to open sockets rather than running a stack of its
# own. It runs ESP-Hosted now -- it is a radio and myrtos does the networking,
# through modules/ehspi and kernel/ehnet.c -- and the WiFiNINA protocol is not
# something that chip can answer any more. `wifi` reported "handshake never
# moved" from the moment the new firmware went in, which is the honest answer
# and not a useful one.
#
# Left here commented rather than deleted, because the sources are still in the
# tree and this is what it takes to have them back: uncomment these two lines
# and the two names in MYRTOS_RESIDENT below, and flash NINA to the C6 with
# Adafruit's SerialESPPassthrough UF2. docs/esp-hosted/README.md has that
# route; it does not involve myrtos at all.
#myrtos_add_module(wifilib modules/wifilib/wifilib.c LIBRARY)
myrtos_add_module(fat32lib modules/fat32lib/fat32lib.c LIBRARY)

# The SD card driver, kernel/sdcard.c until 7 Sep 2026, with the vendored SDIO
# driver from pico-extras alongside it. Together they were 11.6 kB of a kernel
# that is copied into SRAM at boot and stays there for good.
#
# It is the one module that compiles against the SDK's headers. The PIO, DMA and
# SPI register layouts live there and are fixed addresses that travel with
# nobody; the real functions come from the kernel's table through sdk_shim.c.
# Taking the headers from os_kernel itself, rather than listing them, is what
# keeps the two builds compiling the vendored driver against the same thing.
#
# printf.c comes along because it only ever existed for this driver's error
# paths -- it went where its one caller went.
myrtos_add_module(sdlib modules/sdlib/sdlib.c LIBRARY
                  modules/sdlib/sdk_shim.c
                  modules/sdlib/printf.c
                  third_party/pico_sd_card/sd_card.c)
target_include_directories(sdlib_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(sdlib_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
pico_generate_pio_header(sdlib_app ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/sd_card.pio)

# The serial terminal driver, the first MYRTOS_TYPE_DRIVER module. It was the
# first driver in kernel/io.c and the note above it said for weeks that lifting
# it out was the next step. Like sdlib it compiles against the SDK's headers --
# uart_putc_raw is inline and comes along -- and takes uart_init from the table.
#
# The module is called uart because that is the name the descriptor already
# used: a descriptor's driver_name is a module name now, which is the whole
# point. termdesc has said "uart" since it was written and did not change.
# The five RGB LEDs. PIO2, because PIO0 is the USB host and PIO1 is the SD card
# -- the first thing on this board to want the RP2350's third block.
myrtos_add_module(neopixel modules/neopixel/neopixel.c DRIVER)
target_include_directories(neopixel_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(neopixel_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
pico_generate_pio_header(neopixel_app ${CMAKE_CURRENT_SOURCE_DIR}/modules/neopixel/ws2812.pio)
myrtos_add_data_module(leddesc modules/neopixel/led_desc.c)

# The I2C bus, on the pins the Stemma QT connector uses.
myrtos_add_module(i2cbus modules/i2cbus/i2cbus.c DRIVER)
target_include_directories(i2cbus_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(i2cbus_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
myrtos_add_data_module(i2cdesc modules/i2cbus/i2c_desc.c)

# The touch screen on the Waveshare panel. The chip is in the driver and the
# board is in the descriptor, which is what lets the same gt911.c serve a panel
# wired differently.
myrtos_add_module(gt911 modules/gt911/gt911.c DRIVER)
target_include_directories(gt911_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(gt911_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
myrtos_add_data_module(touchdesc modules/gt911/touch_desc.c)
myrtos_add_module(touch modules/touchcmd/touch.c)
myrtos_add_module(i2c modules/i2c/i2c.c)
myrtos_add_module(tone modules/tone/tone.c)
myrtos_add_module(volume modules/volume/volume.c)
myrtos_add_module(play modules/play/play.c)
myrtos_add_module(adc modules/adcread/adcread.c)
myrtos_add_module(gpio modules/gpiocmd/gpiocmd.c)
myrtos_add_module(crit modules/crit/crit.c)

# I2S out. PIO2 state machine 1 -- the neopixel driver has 0, and a block has
# four state machines and thirty-two instruction slots between them.
myrtos_add_module(i2sout modules/audio/audio.c DRIVER)
myrtos_add_module(gpiodev modules/gpio/gpio.c DRIVER)
target_include_directories(gpiodev_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(gpiodev_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
myrtos_add_data_module(gpiodesc modules/gpio/gpio_desc.c)
myrtos_add_module(adcdev modules/adc/adc.c DRIVER)
target_include_directories(adcdev_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(adcdev_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
target_include_directories(i2sout_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(i2sout_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
pico_generate_pio_header(i2sout_app ${CMAKE_CURRENT_SOURCE_DIR}/modules/audio/audio_i2s.pio)
myrtos_add_data_module(audiodesc modules/audio/audio_desc.c)
myrtos_add_data_module(adcdesc modules/adc/adc_desc.c)

myrtos_add_module(uart modules/uart/uart.c DRIVER)
target_include_directories(uart_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(uart_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)

# The wire to the ESP32-C6, which is UART1 on GP8/GP9 plus its reset and its
# download strap. Not the uart driver with other pins: that one is send-only
# and turns \n into \r\n, and a flash image is not text.
myrtos_add_module(esplink modules/esplink/esplink.c DRIVER)
target_include_directories(esplink_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(esplink_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
myrtos_add_data_module(espdesc modules/esplink/esp_desc.c)
myrtos_add_module(espflash modules/espflash/espflash.c)

# ESP-Hosted's SPI transport. The four pins WiFiNINA used, plus the two the
# co-processor drives to say when it may be clocked and whether it has
# anything to say.
myrtos_add_module(ehspi modules/ehspi/ehspi.c DRIVER)
target_include_directories(ehspi_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(ehspi_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
myrtos_add_data_module(ehdesc modules/ehspi/eh_desc.c)
myrtos_add_module(ehstat modules/ehstat/ehstat.c)
myrtos_add_module(ping modules/ping/ping.c)
myrtos_add_module(browse modules/browse/browse.c)
myrtos_add_module(ehrpc modules/ehrpc/ehrpc.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/third_party/pico_sd_card/sd_card.c
    TARGET_DIRECTORY sdlib_app
    PROPERTIES COMPILE_DEFINITIONS "printf=myrtos_printf")

# Atto, an emacs in two thousand lines of public-domain C, built as an ordinary
# module. See modules/atto/README.md for what a C program needs to get here.
file(GLOB MYRTOS_ATTO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/modules/atto/upstream/*.c)
myrtos_add_module(atto modules/atto/compat.c
                  ${MYRTOS_ATTO_SOURCES}
                  ${CMAKE_CURRENT_SOURCE_DIR}/modules/wasm/examples/curses/curses.c
                  NEWLIB)
target_include_directories(atto_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/wasm/examples/curses)
target_compile_options(atto_app PRIVATE -std=gnu17 -funsigned-char)
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
myrtos_add_module(hibouair modules/hibouair/hibouair.c)

# The WebAssembly host. SINGLE because wasm3's interpreter is tables of function
# pointers and a shareable module may not hold one; at a fixed address in PSRAM
# they are correct, and newlib may be linked as it stands. See modules/wasm.
myrtos_add_module(wasm modules/wasm/wasm.c
    modules/wasm/newlib_glue.c
    modules/wasm/wasi.c
    third_party/wasm3/m3_bind.c
    third_party/wasm3/m3_code.c
    third_party/wasm3/m3_compile.c
    third_party/wasm3/m3_core.c
    third_party/wasm3/m3_env.c
    third_party/wasm3/m3_exec.c
    third_party/wasm3/m3_function.c
    third_party/wasm3/m3_module.c
    third_party/wasm3/m3_parse.c
    third_party/wasm3/m3_validate.c
    SINGLE)
target_include_directories(wasm_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm3)
target_link_libraries(wasm_app PRIVATE m c gcc)
# The two forty-kilobyte compiler structures stay out of the stack. A SINGLE
# module may have writable data, so this costs nothing here -- and mem_size is
# capped at 64 kB, which is less than one of them.
target_compile_definitions(wasm_app PRIVATE d_m3PreferStaticAlloc=1
# 8000 is wasm3's default and costs 40 kB in each of M3Compilation and
# M3Runtime, because both hold arrays of that many entries. 1024 brings them to
# under six, and it is the deepest operand stack a single wasm function may use
# -- generous for anything a person writes by hand.
    d_m3MaxFunctionStackHeight=1024)

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

# Retired with the NINA firmware -- see the note beside wifilib above.
#myrtos_add_module(wifi modules/wifi/wifi.c)
myrtos_add_module(pwd modules/pwd/pwd.c)
myrtos_add_module(date modules/date/date.c)
myrtos_add_module(fetch modules/fetch/fetch.c)
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
            ${CMAKE_CURRENT_BINARY_DIR}/vtdemo.mod
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
            ${CMAKE_CURRENT_BINARY_DIR}/date.mod
            ${CMAKE_CURRENT_BINARY_DIR}/fetch.mod
            ${CMAKE_CURRENT_BINARY_DIR}/gt911.mod
            ${CMAKE_CURRENT_BINARY_DIR}/touchdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/touch.mod
            # wifi.mod: retired with the NINA firmware, see the note above. An old
            # build tree still had the file, so only a clean one saw this.
            ${CMAKE_CURRENT_BINARY_DIR}/lwipd.mod
            ${CMAKE_CURRENT_BINARY_DIR}/rmdir.mod
            ${CMAKE_CURRENT_BINARY_DIR}/mount.mod
            ${CMAKE_CURRENT_BINARY_DIR}/msgtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/msgcli.mod
            ${CMAKE_CURRENT_BINARY_DIR}/font.mod
            ${CMAKE_CURRENT_BINARY_DIR}/color.mod
            ${CMAKE_CURRENT_BINARY_DIR}/kill.mod
            ${CMAKE_CURRENT_BINARY_DIR}/cu.mod
            ${CMAKE_CURRENT_BINARY_DIR}/hibouair.mod
            ${CMAKE_CURRENT_BINARY_DIR}/keys.mod
            ${CMAKE_CURRENT_BINARY_DIR}/memtest.mod
            ${CMAKE_CURRENT_BINARY_DIR}/pimpl.mod
            ${CMAKE_CURRENT_BINARY_DIR}/bootsel.mod
            ${CMAKE_CURRENT_BINARY_DIR}/termdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/kbddesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/condesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/acmdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/espdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/ehdesc.mod)

# What lives in flash rather than on the card.
#
# A module in flash runs where it lies. One read from the card is copied into
# RAM at startup and stays there, so the card is for what is being worked on,
# and this is for the system. The order is the order of the image, which is the
# order lsmod reports; it has no other meaning.
#
# lwipd is deliberately absent: it is a SINGLE module linked to a fixed address
# and belongs nowhere near a concatenated image.
# wasm is resident again only while the fault is being chased -- it crashes the
# board when run, see the note in modules/wasm/wasm.c. Take it out before
# leaving the machine in a state anyone else will type at.
set(MYRTOS_RESIDENT
    vidstat
    cxxdemo
    vtdemo
    wasm
    hibouair
    armtest
    usbdisk
    reboot
    dhello
    tlsftest
    more
    libctest
    newlibtest
#    wifilib
    uart
    esplink
    espflash
    ehspi
    ehstat
    ehrpc
    ping
    browse
    neopixel
    i2cbus
    i2c
    i2sout
    adcdev
    adc
    gpiodev
    gpio
    crit
    tone
    volume
    play
    leds
    httpd
    sockstat
    rhello
    mv
    atto
    head
    # Descriptors first. They are what the devices are made of, and a directory
    # that fills up turns away whatever comes last -- which twice now has been
    # these, leaving a machine with no console to print the complaint on.

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
    date
    fetch
    mount
#    wifi
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
    usbstat
    pecho
    zhello
    msgtest
    msgcli

    # And the board's own: descriptors and libraries for hardware that only
    # exists on one machine. They left this list when there were two boards,
    # because several of them would drive the wrong pins on the other one --
    # i2cdesc is GP20/21, which on the Waveshare panel are DE and VSYNC, and
    # leddesc and audiodesc sit on its data lines. Chosen where the board is.
    ${myrtosBoardModules}
)
# What is resident is the intersection of what was asked for and what exists.
#
# MYRTOS_RESIDENT names modules; whether a module is built depends on whether
# its toolchain is on this machine. Asking for one that was skipped used to end
# in "missing and no known rule to make it", which says nothing about ldc2 not
# being installed -- and it would have said that on a RISC-V machine without
# ldc2 just as readily as on ARM. Names are dropped here, and said out loud.
get_property(myrtos_built GLOBAL PROPERTY MYRTOS_BUILT_MODULES)
set(resident_mods "")
set(resident_absent "")
foreach(m ${MYRTOS_RESIDENT})
    if(m IN_LIST myrtos_built)
        list(APPEND resident_mods ${CMAKE_CURRENT_BINARY_DIR}/${m}.mod)
    else()
        list(APPEND resident_absent ${m})
    endif()
endforeach()
if(resident_absent)
    message(STATUS "Not resident, not built here: ${resident_absent}")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/modules.bin
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/make_flash_image.py
            ${CMAKE_CURRENT_BINARY_DIR}/modules.bin ${resident_mods}
    DEPENDS ${resident_mods} ${CMAKE_CURRENT_SOURCE_DIR}/make_flash_image.py
    COMMENT "Building the resident module image")

add_custom_target(flashimage ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/modules.bin)
add_dependencies(flashimage modules)

# --- One file for the whole machine ----------------------------------------
#
# The kernel and the module image are written to two addresses a megabyte apart,
# which used to mean two files and two chances to load only one of them. A UF2
# block carries its own target address, so both fit in one file with a gap in
# the middle and the bootrom puts each block where it says.
#
# myrtos.uf2 is the system. An application built elsewhere against the SDK has
# its own image at MYRTOS_FLASH_APP_BASE and its own UF2, and tools/combine_uf2
# .py takes that and this and makes the one file a product is shipped as -- or
# the application's alone, when only the application has changed.
#
# The address is read out of the header rather than written again here: there is
# one place MYRTOS_FLASH_MODULE_BASE is defined and the kernel is it.
file(STRINGS kernel/flashmod.h myrtosFlashBaseLine
     REGEX "define[ \t]+MYRTOS_FLASH_MODULE_BASE")
string(REGEX MATCH "0x[0-9a-fA-F]+" myrtosFlashBase "${myrtosFlashBaseLine}")
if(NOT myrtosFlashBase)
    message(FATAL_ERROR "MYRTOS_FLASH_MODULE_BASE is not in kernel/flashmod.h "
                        "in a form this can read")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/system_modules.uf2
    COMMAND $<TARGET_FILE:picotool> uf2 convert --quiet
            ${CMAKE_CURRENT_BINARY_DIR}/modules.bin -t bin
            -o ${myrtosFlashBase}
            ${CMAKE_CURRENT_BINARY_DIR}/system_modules.uf2
            --family ${MYRTOS_UF2_FAMILY}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/modules.bin
    COMMENT "Wrapping the module image for ${myrtosFlashBase}")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/myrtos.uf2
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/combine_uf2.py
            ${CMAKE_CURRENT_BINARY_DIR}/myrtos.uf2
            ${CMAKE_CURRENT_BINARY_DIR}/os_kernel.uf2
            ${CMAKE_CURRENT_BINARY_DIR}/system_modules.uf2
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/system_modules.uf2 os_kernel
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/combine_uf2.py
    COMMENT "Combining the kernel and the module image into myrtos.uf2")

add_custom_target(productimage ALL
                  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/myrtos.uf2)

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
