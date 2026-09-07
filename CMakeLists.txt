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
set(MYRTOS_ARCH riscv CACHE STRING "riscv (Hazard3) or arm (Cortex-M33)")
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

# What a module is compiled and linked with that only one machine understands.
#
# -mcmodel=medany makes RISC-V address its own statics PC-relatively, which is
# what let a module be loaded anywhere before the loader could relocate. ARM has
# no code model to choose: Thumb-2 reaches globals through literal pools, and
# those are the absolute words the loader now fixes.
#
# --no-relax is not an optimisation setting, and the note further down says why.
# It is the RISC-V linker's, and arm-none-eabi's ld does not know the option at
# all. -m elf32lriscv likewise: the driver picks its own emulation, so ARM is
# given nothing and gets it right.
#
# Every module link goes through this variable, the D and Zig rules included.
# They used to spell -m elf32lriscv out for themselves, which is how they came
# to be riscv-only: the flag was the thing that could not follow. A side effect
# of routing them here is that D now gets --no-relax as well, which it always
# should have had -- relaxation is what turns an absolute reference into a
# gp-relative one, and a module may not contain either.
#
# The clang, D and Zig rules drive their own compiler and linker rather than the
# SDK's gcc driver, so they need the same two facts spelled without the -Wl,
# wrapper: which triple to compile for and which emulation to link with. They
# are set here, beside the flags they belong to, because the reason the clang
# path was riscv-only was that these were written into it.
if(MYRTOS_ARCH STREQUAL "riscv")
    set(MYRTOS_MODULE_CFLAGS  -mcmodel=medany)
    set(MYRTOS_MODULE_LDFLAGS -Wl,-m,elf32lriscv -Wl,--no-relax)
    set(MYRTOS_MODULE_ARCH_SOURCES)
    set(MYRTOS_LLVM_TRIPLE    riscv32-unknown-elf)
    set(MYRTOS_LLVM_EMULATION elf32lriscv)
    set(MYRTOS_LLVM_CFLAGS)
else()
    # -ffixed-r9 keeps the register the compiler may not have. r9 carries the
    # thread pointer, which RISC-V has in tp and ARM has nowhere -- so the
    # platform register holds it and the compiler must be told to leave it be.
    # Without this a module reads someone else's thread-local eventually.
    set(MYRTOS_MODULE_CFLAGS  -ffixed-r9)
    set(MYRTOS_MODULE_LDFLAGS)

    # And the function the compiler calls to read it. __thread on ARM is not an
    # instruction but a call to __aeabi_read_tp, which the platform supplies --
    # so every module carries the two instructions that answer it.
    set(MYRTOS_MODULE_ARCH_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/common/arm/tp.S)

    set(MYRTOS_LLVM_TRIPLE    arm-none-eabi)
    set(MYRTOS_LLVM_EMULATION armelf)

    # -mno-movt, and it is the whole reason clang could not build ARM modules.
    #
    # To put a 32-bit address in a register, gcc emits a literal pool entry --
    # a word of data inside .text, which the linker fills in and which arrives
    # as R_ARM_ABS32. The loader has always relocated those. Clang prefers a
    # movw/movt pair instead, splitting the same address across the immediate
    # fields of two Thumb instructions, which arrives as R_ARM_THM_MOVW_ABS_NC
    # and R_ARM_THM_MOVT_ABS. Those are absolute too, and check_module.py
    # refused them -- correctly, because nothing relocates them.
    #
    # Two ways out: teach the loader to split a value across two instruction
    # encodings, or ask the compiler for the form the loader already handles.
    # This is the second, it is one flag, and the cost is the literal pool gcc
    # pays anyway. cxxdemo came out with nine addresses to fix and no refusal.
    set(MYRTOS_LLVM_CFLAGS -mno-movt)
endif()

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion ${myrtosToolchain})
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# ====================================================================================
# Select the development board for your hardware
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")

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
    kernel/video.c
    kernel/console.c
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

# The SD card driver is not built into the kernel any more. It is the sdlib
# module -- see the myrtos_add_module call for it further down, and the note at
# the top of kernel/sdlink.c.

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
        set(single_link "")   # relocated like every other module
        list(REMOVE_ITEM ARGN "SINGLE")
    endif()

    # A trailing NEWLIB gives the module the whole C library instead of the
    # header-only subset in common/myrtos_stdio.h and its neighbours. That
    # subset is smaller and links nothing; this is for ported code, where the
    # library is what the program was written against.
    #
    # nano.specs is newlib-nano: the same library with the space-hungry parts
    # left out. Measured on Atto, it costs about seven kilobytes of code over
    # the hand-written subset and gives qsort, strtod, time, dirent and a stdio
    # nobody has to keep adding to.
    #
    # myrtos_syscalls.c is its bottom end and comes along automatically -- a
    # dozen functions, and the note at the top of it explains why the reentrancy
    # struct needs no work here.
    #
    # Such a module is always copied per process, because newlib's _impure_ptr
    # is writable data. That is what makes it correct, and it is also what it
    # costs: the code is no longer shared between instances.
    # A trailing LIBRARY makes a module the kernel CALLS rather than runs.
    # exec_offset then points at a table instead of an entry point, and the
    # table is always called myrtos_lib -- one name, so the loader never has to
    # be told which symbol to look for. See MYRTOS_TYPE_LIBRARY in the ABI.
    set(lib_flag "")
    set(entry_symbol module_main)
    if("LIBRARY" IN_LIST ARGN)
        set(lib_flag "--library")
        set(entry_symbol myrtos_lib)
        list(REMOVE_ITEM ARGN "LIBRARY")
    endif()

    # A trailing DRIVER is the same idea with a fixed shape: exec_offset points
    # at a myrtos_driver_module_t, always called myrtos_driver, which carries an
    # init and the vtable the I/O manager calls every device through. A
    # descriptor naming this driver finds it whether or not the kernel was built
    # with it. See MYRTOS_TYPE_DRIVER in the ABI.
    if("DRIVER" IN_LIST ARGN)
        set(lib_flag "--driver")
        set(entry_symbol myrtos_driver)
        list(REMOVE_ITEM ARGN "DRIVER")
    endif()

    set(libc_compile -ffreestanding -nostdlib)
    set(libc_link    -nostdlib)
    set(libc_sources "")
    if("NEWLIB" IN_LIST ARGN)
        set(libc_compile --specs=nano.specs)
        set(libc_link    --specs=nano.specs)
        set(libc_sources ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_syscalls.c
                         ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_dirent.c)
        list(REMOVE_ITEM ARGN "NEWLIB")
    endif()

    add_executable(${name}_app ${source} ${ARGN}
                   ${libc_sources} ${MYRTOS_MODULE_ARCH_SOURCES})
    # common/newlib holds the headers newlib does not have. It goes first on the
    # include path so that <dirent.h> finds ours rather than newlib's, whose
    # entire content is #error "<dirent.h> not supported".
    if(libc_sources)
        target_include_directories(${name}_app BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/common/newlib
            ${CMAKE_CURRENT_SOURCE_DIR}/common)
    endif()
    target_compile_options(${name}_app PRIVATE
        -fno-pic ${MYRTOS_MODULE_CFLAGS} -fno-common ${libc_compile} -O2
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
    # --no-relax, and it is not an optimisation setting. The RISC-V linker
    # relaxes any access within reach of __global_pointer$ into a gp-relative
    # one, and a module's gp is not its own: the kernel starts every process
    # with the kernel's gp, so a relaxed access reads and writes the kernel's
    # small-data area instead of the module's. The wasm host had 752 of them
    # and behaved exactly as that implies -- a constant read back as the wrong
    # number, pointers that were not pointers, and a machine that fell over.
    target_link_options(${name}_app PRIVATE ${libc_link} ${MYRTOS_MODULE_LDFLAGS} -Wl,-N
                        -Wl,-e,0 -Wl,--no-warn-rwx-segments -Wl,--emit-relocs
                        ${single_link})
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
                ${entry_symbol}
                ${rt_flag} ${lib_flag}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_SOURCE_DIR}/make_module.py
                ${CMAKE_CURRENT_SOURCE_DIR}/common/myrtos_abi.h
        COMMENT "Building module ${name}.mod")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in MYRTOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY MYRTOS_BUILT_MODULES ${name})
endfunction()

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
myrtos_add_module(wifilib modules/wifilib/wifilib.c LIBRARY)
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
myrtos_add_module(uart modules/uart/uart.c DRIVER)
target_include_directories(uart_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,INCLUDE_DIRECTORIES>)
target_compile_definitions(uart_app PRIVATE
    $<TARGET_PROPERTY:os_kernel,COMPILE_DEFINITIONS>)
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
            ${CMAKE_CURRENT_BINARY_DIR}/hibouair.mod
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
# wasm is resident again only while the fault is being chased -- it crashes the
# board when run, see the note in modules/wasm/wasm.c. Take it out before
# leaving the machine in a state anyone else will type at.
set(MYRTOS_RESIDENT
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
    wifilib
    fat32lib
    sdlib
    uart
    httpd
    atto
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
    usbstat
    pecho
    zhello
    msgtest
    msgcli
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
