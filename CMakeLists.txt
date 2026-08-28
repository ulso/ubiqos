# ====================================================================================
# Sätt rätt utvecklingskort för din hårdvara
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")

# Tvinga hela Pico SDK att bygga för RP2350 i RISC-V läge!
set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico platform")

cmake_minimum_required(VERSION 3.16)

# Ladda Pico SDK
include(pico_sdk_import.cmake)

project(myrtos C CXX ASM)

pico_sdk_init()

# ============================================================================
# 1. BYGG OPERATIVSYSTEMETS KÄRNA (myrtos)
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

# Körningen sker i RAM, men imagen ligger i flash och kopieras dit vid boot.
# Det är avsiktligt: boot-ROM:et läser PICOBIN_IMAGE_TYPE_EXE_CPU ur imagen och
# växlar till RISC-V först då. Med no_flash finns ingen image att växla på, så
# chippet startar i ARM och felsökarens reset-och-halt hittar ingen RISC-V-kärna.
pico_set_binary_type(os_kernel copy_to_ram)

# Länka med standardbibliotek för Pico och aktivera PSRAM-stöd
# pico_standard_link ger bara länkskript och länkflaggor. Startkoden crt0,
# som definierar _entry_point och anropar main, kommer med pico_runtime.
# hardware_uart och hardware_gpio för diagnostikporten.
target_link_libraries(os_kernel pico_runtime hardware_uart hardware_gpio hardware_spi pico_time
    tinyusb_device)

# TinyUSB letar tusb_config.h i inkluderingsvägen.
target_include_directories(os_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/kernel)

# Aktivera extra varningar och optimering för RTOS
target_compile_options(os_kernel PRIVATE -Wall -O2)

# Skapa uf2-fil m.m. för laddning till Fruit Jam
pico_add_extra_outputs(os_kernel)

# ============================================================================
# MODULBYGGE
# ============================================================================
# .mod-filen är en riktig OUTPUT och inte ett POST_BUILD-steg. Skillnaden är
# att den byggs om även när make_module.py ändras, inte bara när modulens
# källkod gör det.
function(myrtos_add_module name source)
    add_executable(${name}_app ${source})
    target_compile_options(${name}_app PRIVATE
        -fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
    target_link_options(${name}_app PRIVATE -nostdlib -Wl,-m,elf32lriscv)

    # Positionsoberoendet kontrolleras innan modulen paketeras. En absolut
    # adress i en allokerad sektion -- en statisk funktionspekare, en vtable,
    # en pekare till en statisk variabel -- gör att modulen inte kan laddas på
    # en okänd adress, och det syns inte i källkoden.
    add_custom_command(TARGET ${name}_app POST_BUILD
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_module.py
                ${CMAKE_READELF}
                $<TARGET_OBJECTS:${name}_app>
                $<TARGET_FILE:${name}_app>
        COMMAND_EXPAND_LISTS
        COMMENT "Kontrollerar att ${name} är positionsoberoende")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary
                $<TARGET_FILE:${name}_app>
                ${CMAKE_CURRENT_BINARY_DIR}/${name}_app.bin
        DEPENDS ${name}_app
        COMMENT "Extraherar rå positionsoberoende maskinkod ur ${name}")

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

# En datamodul har ingen startpunkt: den ÄR sina byte.
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
        COMMENT "Kontrollerar att ${name} är positionsoberoende")

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
            ${CMAKE_CURRENT_BINARY_DIR}/termdesc.mod
            ${CMAKE_CURRENT_BINARY_DIR}/usbdesc.mod)
