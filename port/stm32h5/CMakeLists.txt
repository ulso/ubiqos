# The STM32H5 port's own build, while the port is being brought up. The kernel
# joins it here once the board has a working clock and console; until then this
# builds the test program and nothing of the pico-sdk.
#
#   cmake -G Ninja -S port/stm32h5 -B build-h5
#   ninja -C build-h5
#   probe-rs download --chip STM32H563ZITx build-h5/h5hello.elf && probe-rs reset --chip STM32H563ZITx

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

# The RP2350's Arm flags, less -mcmse: that chip runs its code in the secure
# state, and this one, with TrustZone off, has none. softfp keeps a module built
# for one calling the same way on the other.
set(H5_CPU_FLAGS -mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp)

add_executable(h5hello
    hello.c
    startup.c
    clock.c
    console.c
)
target_compile_definitions(h5hello PRIVATE STM32H563xx)
target_include_directories(h5hello PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${UBIQOS_ROOT}/third_party/cmsis-device-h5/Include
    ${UBIQOS_ROOT}/third_party/cmsis-core/Include
)
target_compile_options(h5hello PRIVATE ${H5_CPU_FLAGS} -O2 -g -Wall -Wextra
    -ffunction-sections -fdata-sections)
target_link_options(h5hello PRIVATE ${H5_CPU_FLAGS} -nostartfiles --specs=nano.specs
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32h563zi.ld -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/h5hello.map)
set_target_properties(h5hello PROPERTIES SUFFIX ".elf")

# --- The kernel ---------------------------------------------------------------
#
# The same kernel sources the RP2350 build compiles, less everything that has no
# hardware here yet: video, USB, the card, WiFi, the key store. What they
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

# lwIP itself, from the same copy the RP2350 kernel is built with: the one in
# the pico-sdk's tree, which is lwIP 2.2 as released. Nothing else of the SDK is
# used; lib/lwip-patched has UbiqOS's two mDNS files, and port/stm32h5/lwip has
# the arch header the SDK would otherwise supply.
set(UBIQOS_LWIP_DIR "$ENV{HOME}/.pico-sdk/sdk/2.3.1/lib/lwip" CACHE PATH "lwIP's source tree")
file(GLOB H5_LWIP_SOURCES
    ${UBIQOS_LWIP_DIR}/src/core/*.c
    ${UBIQOS_LWIP_DIR}/src/core/ipv4/*.c)
list(APPEND H5_LWIP_SOURCES ${UBIQOS_LWIP_DIR}/src/netif/ethernet.c)
add_executable(ubiqos_h5
    ${K}/crashlog.c
    ${K}/vfs.c
    ${K}/tmpfs.c
    ${K}/varfs.c
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
    ${K}/config.c
    ${K}/clock.c
    ${K}/fsserver.c
    ${K}/console_none.c
    ${K}/arm/scheduler.S
    ${K}/arm/stack.c
    startup.c
    clock.c
    console.c
    timer.c
    sdk.c
    stubs.c
    kapi.c
    eth.c
    ethnet.c
    flash.c
    ${K}/keystore.c
    ${K}/crypto.c
    ${K}/lwipsock.c
    ${K}/lwipping.c
    ${K}/lwipmdns.c
    ${K}/lwipsntp.c
    ${UBIQOS_ROOT}/lib/lwip-patched/apps/mdns/mdns.c
    ${UBIQOS_ROOT}/lib/lwip-patched/apps/mdns/mdns_out.c
    ${UBIQOS_LWIP_DIR}/src/apps/mdns/mdns_domain.c
    ${H5_LWIP_SOURCES}
)
add_dependencies(ubiqos_h5 ubiqos_version)
target_include_directories(ubiqos_h5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/lwip
    ${UBIQOS_LWIP_DIR}/src/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${UBIQOS_ROOT}
    ${K}
    ${UBIQOS_ROOT}/common
    ${UBIQOS_ROOT}/third_party/cmsis-device-h5/Include
    ${UBIQOS_ROOT}/third_party/cmsis-core/Include
)
target_compile_definitions(ubiqos_h5 PRIVATE
    STM32H563xx
    UBIQOS_CHIP_STM32H5=1
    "UBIQOS_BOARD_HEADER=<boards/nucleo-h563zi.h>"
    UBIQOS_HAS_VIDEO=0 UBIQOS_HAS_CONSOLE=0
    UBIQOS_HEAP_SIZE=458752
    DMESG_SIZE=16384
    UBIQOS_CRITICAL_BASEPRI=0x80
    UBIQOS_LWIP=1
)
target_compile_options(ubiqos_h5 PRIVATE ${H5_CPU_FLAGS} -O2 -g -Wall
    -ffunction-sections -fdata-sections -fno-math-errno)
target_link_options(ubiqos_h5 PRIVATE ${H5_CPU_FLAGS} -nostartfiles --specs=nano.specs
    -T${CMAKE_CURRENT_SOURCE_DIR}/stm32h563zi.ld -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/ubiqos_h5.map)
set_target_properties(ubiqos_h5 PROPERTIES SUFFIX ".elf")
