#!/bin/bash
# Build the ESP-Hosted co-processor firmware for iLabs' Challenger+ RP2350's
# ESP32-C6: esp/fruitjam-c6/build.sh, with this board's wiring and a work
# directory of its own, since the two builds share nothing but the checkout.
#
#   . ~/esp/esp-idf-v5.5.5/export.sh
#   esp/challenger-c6/build.sh [WORK] [ble]
#
# With ble, Espressif's WiFi-and-Bluetooth co-processor instead of WiFi alone:
# the BLE controller on the C6, its HCI carried over the same SPI link as the
# network (interface type 4), and no host stack on either side of it -- the
# host brings its own. See modules/blescan.
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "${2:-}" = ble ]; then
    export EXAMPLE=bluetooth/esp_hosted_nimble/bleprph_wifi_coex/cp
fi
BOARD_DEFAULTS="$HERE/sdkconfig.defaults.challenger" \
    exec "$HERE/../fruitjam-c6/build.sh" "${1:-/tmp/esp_hosted_challenger}"
