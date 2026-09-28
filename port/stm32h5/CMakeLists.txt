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
