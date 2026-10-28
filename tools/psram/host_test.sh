#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-psram.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
for BSS in 0 1; do
    cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -DM5OS_BOARD_TAB5=1 -DCONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=$BSS \
        -Ikernel/include -Itools/psram/stubs tools/psram/host_test.c \
        hal/esp32p4/mem_psram.c -o "$OUT/psram_$BSS"
    "$OUT/psram_$BSS"
done
