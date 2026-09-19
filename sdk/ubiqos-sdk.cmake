# The UbiqOS SDK: everything needed to build a module, and nothing else.
#
# A module is a relocatable blob with a header, compiled against
# common/ubiqos_abi.h and linked against nothing. Building one correctly takes
# more knowledge than that sentence suggests -- a code model, a reserved
# register, a linker that must not relax, a position-independence check and a
# packaging step -- and all of it used to live in this project's own
# CMakeLists.txt, which meant a module could only be built inside this tree.
#
# It is EXTRACTED here rather than copied. The repository's own build includes
# this file and has no second copy of any of it, so the two cannot drift: if
# what is here were wrong, every module in the tree would be wrong with it.
#
# What a separate repository does with it:
#
#     cmake_minimum_required(VERSION 3.13)
#     set(UBIQOS_ARCH arm)                     # or riscv, before the SDK import
#     include(pico_sdk_import.cmake)           # copied from the UbiqOS tree
#     project(myapp C CXX ASM)
#     pico_sdk_init()
#     include(${UBIQOS_DIR}/sdk/ubiqos-sdk.cmake)
#
#     ubiqos_add_module(myapp src/myapp.c)
#     ubiqos_app_image(app myapp)              # -> app.bin, app.uf2
#
# and nothing of the application needs to be in this repository. The image it
# produces is written to the application region in flash, so it is loaded
# beside the system rather than into it -- see UBIQOS_FLASH_APP_BASE in
# kernel/flashmod.h, and tools/combine_uf2.py for shipping the two as one file.
#
# The Pico SDK is here for its toolchain and for picotool, not for its
# libraries: a module links against none of them.

# Where this file is, and therefore where the headers, the scripts and the
# architecture's helper sources are. CMAKE_CURRENT_LIST_DIR is the directory of
# the file being processed, which is the whole reason this works from outside:
# CMAKE_CURRENT_SOURCE_DIR would be the including project's.
set(UBIQOS_SDK_DIR ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(UBIQOS_ROOT ${UBIQOS_SDK_DIR} DIRECTORY)

if(NOT DEFINED UBIQOS_ARCH)
    message(FATAL_ERROR
            "UBIQOS_ARCH must be set before the Pico SDK is imported: it decides "
            "the toolchain, and the SDK settles that at import. riscv or arm.")
endif()

# The UF2 family is the chip and the machine together. Loading the wrong one is
# refused by the bootrom, but it is better refused at the point where the two
# builds would otherwise produce identically named files.
if(UBIQOS_ARCH STREQUAL "riscv")
    set(UBIQOS_UF2_FAMILY rp2350-riscv)
else()
    set(UBIQOS_UF2_FAMILY rp2350-arm-s)
endif()

# The application region's address, read out of the kernel's own header so that
# there is one place it is written down.
file(STRINGS ${UBIQOS_ROOT}/kernel/flashmod.h ubiqosAppBaseLine
     REGEX "define[ \t]+UBIQOS_FLASH_APP_BASE")
string(REGEX MATCH "0x[0-9a-fA-F]+" UBIQOS_FLASH_APP_BASE "${ubiqosAppBaseLine}")
if(NOT UBIQOS_FLASH_APP_BASE)
    message(FATAL_ERROR "UBIQOS_FLASH_APP_BASE is not in kernel/flashmod.h in a "
                        "form this can read")
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
if(UBIQOS_ARCH STREQUAL "riscv")
    set(UBIQOS_MODULE_CFLAGS  -mcmodel=medany)
    set(UBIQOS_MODULE_LDFLAGS -Wl,-m,elf32lriscv -Wl,--no-relax)
    set(UBIQOS_MODULE_ARCH_SOURCES)
    set(UBIQOS_LLVM_TRIPLE    riscv32-unknown-elf)
    set(UBIQOS_LLVM_EMULATION elf32lriscv)
    set(UBIQOS_LLVM_CFLAGS)
else()
    # -ffixed-r9 keeps the register the compiler may not have. r9 carries the
    # thread pointer, which RISC-V has in tp and ARM has nowhere -- so the
    # platform register holds it and the compiler must be told to leave it be.
    # Without this a module reads someone else's thread-local eventually.
    set(UBIQOS_MODULE_CFLAGS  -ffixed-r9)
    set(UBIQOS_MODULE_LDFLAGS)

    # And the function the compiler calls to read it. __thread on ARM is not an
    # instruction but a call to __aeabi_read_tp, which the platform supplies --
    # so every module carries the two instructions that answer it.
    set(UBIQOS_MODULE_ARCH_SOURCES ${UBIQOS_ROOT}/common/arm/tp.S)

    set(UBIQOS_LLVM_TRIPLE    arm-none-eabi)
    set(UBIQOS_LLVM_EMULATION armelf)

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
    set(UBIQOS_LLVM_CFLAGS -mno-movt)
endif()


function(ubiqos_add_module name source)
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

    # A trailing AUTOSTART starts the program when the system comes up, card or
    # no card. See UBIQOS_ATTR_AUTOSTART in the ABI.
    set(autostart_flag "")
    if("AUTOSTART" IN_LIST ARGN)
        set(autostart_flag "--autostart")
        list(REMOVE_ITEM ARGN "AUTOSTART")
    endif()

    # A trailing NEWLIB gives the module the whole C library instead of the
    # header-only subset in common/ubiqos_stdio.h and its neighbours. That
    # subset is smaller and links nothing; this is for ported code, where the
    # library is what the program was written against.
    #
    # nano.specs is newlib-nano: the same library with the space-hungry parts
    # left out. Measured on Atto, it costs about seven kilobytes of code over
    # the hand-written subset and gives qsort, strtod, time, dirent and a stdio
    # nobody has to keep adding to.
    #
    # ubiqos_syscalls.c is its bottom end and comes along automatically -- a
    # dozen functions, and the note at the top of it explains why the reentrancy
    # struct needs no work here.
    #
    # Such a module is always copied per process, because newlib's _impure_ptr
    # is writable data. That is what makes it correct, and it is also what it
    # costs: the code is no longer shared between instances.
    # A trailing LIBRARY makes a module the kernel CALLS rather than runs.
    # exec_offset then points at a table instead of an entry point, and the
    # table is always called ubiqos_lib -- one name, so the loader never has to
    # be told which symbol to look for. See UBIQOS_TYPE_LIBRARY in the ABI.
    set(lib_flag "")
    set(entry_symbol module_main)
    if("LIBRARY" IN_LIST ARGN)
        set(lib_flag "--library")
        set(entry_symbol ubiqos_lib)
        list(REMOVE_ITEM ARGN "LIBRARY")
    endif()

    # A trailing DRIVER is the same idea with a fixed shape: exec_offset points
    # at a ubiqos_driver_module_t, always called ubiqos_driver, which carries an
    # init and the vtable the I/O manager calls every device through. A
    # descriptor naming this driver finds it whether or not the kernel was built
    # with it. See UBIQOS_TYPE_DRIVER in the ABI.
    if("DRIVER" IN_LIST ARGN)
        set(lib_flag "--driver")
        set(entry_symbol ubiqos_driver)
        list(REMOVE_ITEM ARGN "DRIVER")
    endif()

    set(libc_compile -ffreestanding -nostdlib)
    set(libc_link    -nostdlib)
    set(libc_sources "")
    if("NEWLIB" IN_LIST ARGN)
        set(libc_compile --specs=nano.specs)
        set(libc_link    --specs=nano.specs)
        set(libc_sources ${UBIQOS_ROOT}/common/ubiqos_syscalls.c
                         ${UBIQOS_ROOT}/common/ubiqos_dirent.c)
        list(REMOVE_ITEM ARGN "NEWLIB")
    endif()

    add_executable(${name}_app ${source} ${ARGN}
                   ${libc_sources} ${UBIQOS_MODULE_ARCH_SOURCES})
    # So that a module says #include <ubiqos_abi.h> wherever it lives. The
    # modules in this tree reach the same file by a relative path and are
    # unaffected; a module in another repository has no relative path to it.
    target_include_directories(${name}_app PRIVATE ${UBIQOS_ROOT}/common)
    # common/newlib holds the headers newlib does not have. It goes first on the
    # include path so that <dirent.h> finds ours rather than newlib's, whose
    # entire content is #error "<dirent.h> not supported".
    if(libc_sources)
        target_include_directories(${name}_app BEFORE PRIVATE
            ${UBIQOS_ROOT}/common/newlib
            ${UBIQOS_ROOT}/common)
    endif()
    target_compile_options(${name}_app PRIVATE
        -fno-pic ${UBIQOS_MODULE_CFLAGS} -fno-common ${libc_compile} -O2
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
    target_link_options(${name}_app PRIVATE ${libc_link} ${UBIQOS_MODULE_LDFLAGS} -Wl,-N
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
        COMMAND python3 ${UBIQOS_ROOT}/check_module.py
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
        COMMAND python3 ${UBIQOS_ROOT}/make_module.py
                ${single_flag}
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.mod
                "${name}"
                $<TARGET_FILE:${name}_app>
                ${CMAKE_NM}
                ${entry_symbol}
                ${rt_flag} ${lib_flag} ${autostart_flag}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
                ${UBIQOS_ROOT}/make_module.py
                ${UBIQOS_ROOT}/common/ubiqos_abi.h
        COMMENT "Building module ${name}.mod")

    # Built, and therefore able to be resident. A module whose toolchain is
    # not on this machine never reaches here, and the flash image asks this
    # list rather than assuming every name in UBIQOS_RESIDENT exists.
    set_property(GLOBAL APPEND PROPERTY UBIQOS_BUILT_MODULES ${name})
endfunction()


# --- TLS ------------------------------------------------------------------
#
# ubiqos_module_use_tls(name) gives a NEWLIB module a TLS client: lib/tls, and
# the Pico SDK's mbedTLS built as a module would build it. mbedTLS is an
# archive, built once per build tree, so a module links only the parts it
# calls -- the configuration (lib/tls/ubiqos_mbedtls_config.h) is a TLS 1.2
# client and nothing else, and much of the library compiles to nothing.
function(ubiqos_module_use_tls name)
    set(mbedtls_dir ${PICO_SDK_PATH}/lib/mbedtls)
    set(tls_config  -DMBEDTLS_CONFIG_FILE="ubiqos_mbedtls_config.h")
    if(NOT TARGET ubiqos_mbedtls)
        file(GLOB mbedtls_sources ${mbedtls_dir}/library/*.c)
        add_library(ubiqos_mbedtls STATIC ${mbedtls_sources})
        target_include_directories(ubiqos_mbedtls PRIVATE
            ${UBIQOS_ROOT}/lib/tls ${mbedtls_dir}/include ${mbedtls_dir}/library)
        target_compile_options(ubiqos_mbedtls PRIVATE
            -fno-pic ${UBIQOS_MODULE_CFLAGS} -fno-common --specs=nano.specs -O2
            ${tls_config})
    endif()
    target_sources(${name}_app PRIVATE ${UBIQOS_ROOT}/lib/tls/ubiqos_tls.c)
    target_include_directories(${name}_app PRIVATE
        ${UBIQOS_ROOT}/lib/tls ${mbedtls_dir}/include)
    target_compile_options(${name}_app PRIVATE ${tls_config})
    target_link_libraries(${name}_app PRIVATE ubiqos_mbedtls)
endfunction()


# --- Shipping an application ----------------------------------------------
#
# ubiqos_app_image(<name> <module>...) concatenates modules into an image for
# the application region and wraps it as a UF2. The result is a file that can be
# dragged onto a board on its own -- updating the application and touching
# nothing else -- or folded in beside the system's own UF2 with
# tools/combine_uf2.py to make the one file a product ships as.
#
# The modules are named, not globbed, because what goes in a customer's image is
# a decision and not a directory listing.
function(ubiqos_app_image name)
    if(NOT ARGN)
        message(FATAL_ERROR "ubiqos_app_image(${name}) names no modules")
    endif()

    # picotool comes from the Pico SDK and this is idempotent: the SDK guards it
    # so that calling it after pico_add_extra_outputs has already done so is
    # free. An application repository has no reason to call it itself.
    pico_init_picotool()

    set(mods "")
    foreach(m ${ARGN})
        list(APPEND mods ${CMAKE_CURRENT_BINARY_DIR}/${m}.mod)
    endforeach()

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
        COMMAND python3 ${UBIQOS_ROOT}/make_flash_image.py
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin ${mods}
        DEPENDS ${mods} ${UBIQOS_ROOT}/make_flash_image.py
        COMMENT "Building the application image ${name}.bin")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.uf2
        COMMAND $<TARGET_FILE:picotool> uf2 convert --quiet
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin -t bin
                -o ${UBIQOS_FLASH_APP_BASE}
                ${CMAKE_CURRENT_BINARY_DIR}/${name}.uf2
                --family ${UBIQOS_UF2_FAMILY}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
        COMMENT "Wrapping ${name}.bin for ${UBIQOS_FLASH_APP_BASE}")

    add_custom_target(${name}_image ALL
                      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.uf2)
endfunction()
