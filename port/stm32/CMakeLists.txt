# The STM32H5 port's own build: the kernel for an STM32H5 board -- the
# NUCLEO-H563ZI unless UBIQOS_H5_BOARD says otherwise -- without the pico-sdk.
#
#   cmake -G Ninja -S port/stm32 -B build-h5
#   ninja -C build-h5
#   probe-rs download --chip STM32H563ZITx build-h5/ubiqos_h5.elf && probe-rs reset --chip STM32H563ZITx

cmake_minimum_required(VERSION 3.13)

set(UBIQOS_ARM_TOOLCHAIN "$ENV{HOME}/.pico-sdk/toolchain/15_2_Rel1/bin" CACHE PATH
    "where arm-none-eabi-gcc is")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER   ${UBIQOS_ARM_TOOLCHAIN}/arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER ${UBIQOS_ARM_TOOLCHAIN}/arm-none-eabi-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(ubiqos_stm32h5 C ASM)

set(UBIQOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# What every STM32 shares -- the kernel's view of the port, the stand-ins for
# the pico-sdk's headers, the Ethernet glue -- and what one family needs.
set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/common)
set(FAMILY ${CMAKE_CURRENT_SOURCE_DIR}/h5)

# The RP2350's Arm flags, less -mcmse: that chip runs its code in the secure
# state, and this one, with TrustZone off, has none. softfp keeps a module built
# for one calling the same way on the other.
set(H5_CPU_FLAGS -mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp)

# --- The kernel ---------------------------------------------------------------
#
# The same kernel sources the RP2350 build compiles, less everything that has no
# hardware here yet: video, USB, the card, WiFi. What they
# provide to the rest of the kernel is answered by stubs.c, which says per
# function what is missing and why. The pico-sdk calls the remaining files
# make are answered by the headers in sdk/, whose names are the SDK's own so
# that the kernel's includes need not change -- see sdk/README.md.

add_custom_target(ubiqos_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${UBIQOS_ROOT}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ubiqos_version.h
            -P ${UBIQOS_ROOT}/cmake/version.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/ubiqos_version.h)

set(K ${UBIQOS_ROOT}/kernel)

# Which board: its chip, its linker script and its header. The header says the
# rest -- the clock, the console's pins, where things are in flash, how big the
# kernel's tables are.
#
#   cmake -G Ninja -S port/stm32 -B build-h503 -DUBIQOS_H5_BOARD=nucleo-h503rb
set(UBIQOS_H5_BOARD "nucleo-h563zi" CACHE STRING "nucleo-h563zi or nucleo-h503rb")
if(UBIQOS_H5_BOARD STREQUAL "nucleo-h563zi")
    set(H5_CHIP STM32H563xx)
    set(H5_LD   stm32h563zi.ld)
    set(H5_ETH  ON)
    set(H5_NODE OFF)
elseif(UBIQOS_H5_BOARD STREQUAL "nucleo-h503rb")
    set(H5_CHIP STM32H503xx)
    set(H5_LD   stm32h503rb.ld)
    set(H5_ETH  OFF)                       # the part has no Ethernet MAC
    set(H5_NODE ON)                        # 32 kB: a node, not a computer
else()
    message(FATAL_ERROR "UBIQOS_H5_BOARD: no board called ${UBIQOS_H5_BOARD}")
endif()

# lwIP itself, from the same copy the RP2350 kernel is built with: the one in
# the pico-sdk's tree, which is lwIP 2.2 as released. Nothing else of the SDK is
# used; lib/lwip-patched has UbiqOS's two mDNS files, and port/stm32/common/lwip has
# the arch header the SDK would otherwise supply.
set(UBIQOS_LWIP_DIR "$ENV{HOME}/.pico-sdk/sdk/2.3.1/lib/lwip" CACHE PATH "lwIP's source tree")
file(GLOB H5_LWIP_SOURCES
    ${UBIQOS_LWIP_DIR}/src/core/*.c
    ${UBIQOS_LWIP_DIR}/src/core/ipv4/*.c)
list(APPEND H5_LWIP_SOURCES ${UBIQOS_LWIP_DIR}/src/netif/ethernet.c)
add_executable(ubiqos_h5
    ${K}/crashlog.c
    ${K}/memcmp.c
    ${K}/main.c
    ${K}/tlsf.c
    ${K}/scheduler.c
    ${K}/syscalls.c
    ${K}/io.c
    ${K}/pins.c
    ${K}/moddir.c
    ${K}/flashmod.c
    ${K}/net.c
    ${K}/console_none.c
    ${K}/arm/scheduler.S
    ${K}/arm/stack.c
    ${FAMILY}/startup.c
    ${FAMILY}/clock.c
    ${FAMILY}/console.c
    ${FAMILY}/timer.c
    ${FAMILY}/sdk.c
    ${COMMON}/stubs.c
    ${COMMON}/kapi.c
)
# A node or a computer. A node is the scheduler, messages, drivers and the
# modules in flash, all of which it starts at boot and no others: no file
# server, no files, no key store, no wall clock. See UBIQOS_NODE.
set(UBIQOS_H5_NODE ${H5_NODE} CACHE BOOL "the node profile: no files, no shell")
if(UBIQOS_H5_NODE)
    target_sources(ubiqos_h5 PRIVATE ${K}/node.c)
    target_compile_definitions(ubiqos_h5 PRIVATE UBIQOS_NODE=1)
else()
    target_sources(ubiqos_h5 PRIVATE
        ${K}/vfs.c
        ${K}/tmpfs.c
        ${K}/varfs.c
        ${K}/config.c
        ${K}/clock.c
        ${K}/fsserver.c
        ${FAMILY}/flash.c
        ${K}/keystore.c
        ${K}/crypto.c)
    target_compile_definitions(ubiqos_h5 PRIVATE UBIQOS_NODE=0)
endif()
# Ethernet and the stack on it. Off, the kernel is what a board without a
# network would carry -- which is also how to measure what the network costs.
set(UBIQOS_H5_NET ${H5_ETH} CACHE BOOL "Ethernet, lwIP, mDNS and NTP")
if(UBIQOS_H5_NET AND NOT H5_ETH)
    message(FATAL_ERROR "${UBIQOS_H5_BOARD} has no Ethernet: UBIQOS_H5_NET=OFF")
endif()
if(UBIQOS_H5_NET)
    target_sources(ubiqos_h5 PRIVATE
        ${FAMILY}/eth.c
        ${COMMON}/ethnet.c
        ${K}/lwipsock.c
        ${K}/lwipping.c
        ${K}/lwipmdns.c
        ${K}/lwipsntp.c
        ${UBIQOS_ROOT}/lib/lwip-patched/apps/mdns/mdns.c
        ${UBIQOS_ROOT}/lib/lwip-patched/apps/mdns/mdns_out.c
        ${UBIQOS_LWIP_DIR}/src/apps/mdns/mdns_domain.c
        ${H5_LWIP_SOURCES})
    target_compile_definitions(ubiqos_h5 PRIVATE UBIQOS_LWIP=1)
else()
    target_compile_definitions(ubiqos_h5 PRIVATE UBIQOS_LWIP=0)
endif()
add_dependencies(ubiqos_h5 ubiqos_version)
target_include_directories(ubiqos_h5 PRIVATE
    ${FAMILY}
    ${COMMON}
    ${COMMON}/sdk
    ${COMMON}/lwip
    ${UBIQOS_LWIP_DIR}/src/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${UBIQOS_ROOT}
    ${K}
    ${UBIQOS_ROOT}/common
    ${UBIQOS_ROOT}/third_party/cmsis-device-h5/Include
    ${UBIQOS_ROOT}/third_party/cmsis-core/Include
)
target_compile_definitions(ubiqos_h5 PRIVATE
    ${H5_CHIP}
    UBIQOS_CHIP_STM32H5=1
    "UBIQOS_BOARD_HEADER=<boards/${UBIQOS_H5_BOARD}.h>"
    UBIQOS_HAS_VIDEO=0 UBIQOS_HAS_CONSOLE=0
    UBIQOS_POOL_FROM_LINKER=1
    DMESG_SIZE=16384
    UBIQOS_CRITICAL_BASEPRI=0x80
)
target_compile_options(ubiqos_h5 PRIVATE ${H5_CPU_FLAGS} -O2 -g -Wall
    -ffunction-sections -fdata-sections -fno-math-errno)
target_link_options(ubiqos_h5 PRIVATE ${H5_CPU_FLAGS} -nostartfiles --specs=nano.specs
    -T${FAMILY}/${H5_LD} -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ubiqos_h5.map)
set_target_properties(ubiqos_h5 PROPERTIES SUFFIX ".elf")
