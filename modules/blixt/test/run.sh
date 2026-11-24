#!/bin/sh
# Assemble every test/*.s with Blixt and with GNU as, and compare the bytes.
#   modules/blixt/test/run.sh [FILE.s ...]
here=$(cd "$(dirname "$0")" && pwd) || exit 1
out=${TMPDIR:-/tmp}/blixt-asmtest
mkdir -p "$out" || exit 1
cc -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -o "$out/asmtest" \
    "$here/asmtest.c" "$here/../asm.c" || exit 1
[ $# -gt 0 ] || set -- "$here"/*.s
fail=0
for s in "$@"; do
    b=$(basename "$s" .s)
    { printf '.syntax unified\n.thumb\n.text\n'; cat "$s"; } > "$out/$b.gas.s" || exit 1
    arm-none-eabi-as -mcpu=cortex-m33 -mthumb -I "$here" -o "$out/$b.o" "$out/$b.gas.s" || { fail=1; continue; }
    arm-none-eabi-objcopy -O binary -j .text "$out/$b.o" "$out/$b.bin" || exit 1
    "$out/asmtest" "$s" "$out/$b.bin" || fail=1
done
exit $fail
