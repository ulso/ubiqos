# An application in its own repository, built against the UbiqOS SDK.
#
# This is the whole of it. Nothing here is in the UbiqOS tree except the SDK it
# includes, and the image it produces is written to the application region in
# flash -- so the system underneath can be updated without rebuilding this, and
# this without rebuilding the system.
#
#     cmake -G Ninja -S . -B build -DUBIQOS_DIR=/path/to/ubiqos -DUBIQOS_ARCH=arm
#     ninja -C build
#
# leaves build/app.uf2, which can be dragged onto a board on its own. To ship
# the two as one file:
#
#     python3 $UBIQOS_DIR/tools/combine_uf2.py product.uf2 ubiqos.uf2 app.uf2
cmake_minimum_required(VERSION 3.13)

set(UBIQOS_DIR "" CACHE PATH "Where the UbiqOS repository is")
if(NOT UBIQOS_DIR)
    message(FATAL_ERROR "Pass -DUBIQOS_DIR=/path/to/ubiqos")
endif()

# Before the Pico SDK is imported, because the SDK settles its toolchain at
# import and cannot be told afterwards.
set(UBIQOS_ARCH arm CACHE STRING "riscv (Hazard3) or arm (Cortex-M33)")

if(UBIQOS_ARCH STREQUAL "riscv")
    set(PICO_PLATFORM rp2350-riscv)
    set(PICO_TOOLCHAIN_PATH $ENV{HOME}/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_2_0_3)
else()
    set(PICO_PLATFORM rp2350-arm-s)
    set(PICO_TOOLCHAIN_PATH $ENV{HOME}/.pico-sdk/toolchain/15_2_Rel1)
endif()
set(PICO_SDK_PATH $ENV{HOME}/.pico-sdk/sdk/2.3.1)
set(PICO_BOARD pico2 CACHE STRING "")

include(pico_sdk_import.cmake)
project(ubiqos_example C CXX ASM)
pico_sdk_init()

# The SDK brings ubiqos_add_module, the flags a module must be built with, and
# ubiqos_app_image. It brings no libraries: a module links against nothing.
include(${UBIQOS_DIR}/sdk/ubiqos-sdk.cmake)

ubiqos_add_module(hello src/hello.c)
ubiqos_app_image(app hello)
