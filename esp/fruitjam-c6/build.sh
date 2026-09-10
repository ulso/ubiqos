#!/bin/bash
# Build the ESP-Hosted co-processor firmware for the Fruit Jam's ESP32-C6.
#
# Nothing here is myrtos code. It is Espressif's firmware with this board's
# wiring poured into it, and the only thing worth keeping in this repo is the
# wiring and the two traps that cost an afternoon:
#
#   * the checkout directory must be called esp_hosted, not esp-hosted-mcu.
#     ESP-IDF discovers components by DIRECTORY NAME and only afterwards reads
#     the `idf_component_register(NAME esp_hosted)` inside; with the repo's own
#     name the build fails with "Failed to resolve component 'esp_hosted'",
#     which sounds like a missing dependency and is a rename;
#   * the submodules are not optional. protobuf-c lives in one, and without it
#     cmake stops on "No SOURCES given to target: msg_codec".
#
# Needs ESP-IDF v5.5 or later sourced -- esp_hosted 3.0.7 says >= 5.5 and means
# it. Nothing here touches the board.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/esp_hosted_build}
PIN=3450367b247f1881153ca3ea6703e1d82cfb2405   # main, 10 Sep 2026, v3.0.7

if [ -z "$IDF_PATH" ]; then
    echo "source ESP-IDF first, e.g. . ~/esp/esp-idf-v5.5.5/export.sh" >&2
    exit 1
fi

mkdir -p "$WORK"
if [ ! -d "$WORK/esp_hosted" ]; then
    git clone https://github.com/espressif/esp-hosted-mcu.git "$WORK/esp_hosted"
    git -C "$WORK/esp_hosted" checkout "$PIN"
fi
git -C "$WORK/esp_hosted" submodule update --init --recursive --depth 1

CP="$WORK/esp_hosted/examples/wifi/sta/cp"
cd "$CP"
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;$HERE/sdkconfig.defaults.fruitjam" \
       set-target esp32c6 build

echo
echo "Check the wiring reached the image before believing the build:"
grep -E '^CONFIG_EH_TRANSPORT_CP_SPI(_GPIO)?' "$CP/sdkconfig"
echo
echo "Images in $CP/build:"
echo "  0x0     bootloader/bootloader.bin"
echo "  0x8000  partition_table/partition-table.bin"
echo "  0xd000  ota_data_initial.bin"
echo "  0x10000 eh_cp_wifi_sta.bin"
