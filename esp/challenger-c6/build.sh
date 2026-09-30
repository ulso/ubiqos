#!/bin/bash
# Build the ESP-Hosted co-processor firmware for iLabs' Challenger+ RP2350's
# ESP32-C6: esp/fruitjam-c6/build.sh, with this board's wiring and a work
# directory of its own, since the two builds share nothing but the checkout.
#
#   . ~/esp/esp-idf-v5.5.5/export.sh
#   esp/challenger-c6/build.sh [WORK]
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD_DEFAULTS="$HERE/sdkconfig.defaults.challenger" \
    exec "$HERE/../fruitjam-c6/build.sh" "${1:-/tmp/esp_hosted_challenger}"
