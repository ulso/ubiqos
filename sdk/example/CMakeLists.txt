# An application in its own repository, built against the myrtos SDK.
#
# This is the whole of it. Nothing here is in the myrtos tree except the SDK it
# includes, and the image it produces is written to the application region in
# flash -- so the system underneath can be updated without rebuilding this, and
# this without rebuilding the system.
#
#     cmake -G Ninja -S . -B build -DMYRTOS_DIR=/path/to/myrtos -DMYRTOS_ARCH=arm
#     ninja -C build
#
# leaves build/app.uf2, which can be dragged onto a board on its own. To ship
# the two as one file:
#
#     python3 $MYRTOS_DIR/tools/combine_uf2.py product.uf2 myrtos.uf2 app.uf2
cmake_minimum_required(VERSION 3.13)

set(MYRTOS_DIR "" CACHE PATH "Where the myrtos repository is")
if(NOT MYRTOS_DIR)
    message(FATAL_ERROR "Pass -DMYRTOS_DIR=/path/to/myrtos")
endif()

# Before the Pico SDK is imported, because the SDK settles its toolchain at
# import and cannot be told afterwards.
set(MYRTOS_ARCH arm CACHE STRING "riscv (Hazard3) or arm (Cortex-M33)")

if(MYRTOS_ARCH STREQUAL "riscv")
    set(PICO_PLATFORM rp2350-riscv)
    set(PICO_TOOLCHAIN_PATH $ENV{HOME}/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_2_0_3)
else()
    set(PICO_PLATFORM rp2350-arm-s)
    set(PICO_TOOLCHAIN_PATH $ENV{HOME}/.pico-sdk/toolchain/15_2_Rel1)
endif()
set(PICO_SDK_PATH $ENV{HOME}/.pico-sdk/sdk/2.3.1)
set(PICO_BOARD pico2 CACHE STRING "")

include(pico_sdk_import.cmake)
project(airview C CXX ASM)
pico_sdk_init()

# The SDK brings myrtos_add_module, the flags a module must be built with, and
# myrtos_app_image. It brings no libraries: a module links against nothing.
include(${MYRTOS_DIR}/sdk/myrtos-sdk.cmake)

myrtos_add_module(airview src/airview.c)
myrtos_app_image(app airview)
