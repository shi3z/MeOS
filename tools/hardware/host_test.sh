#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-hardware.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
for TEST in power i2c; do
    case "$TEST" in
        power) SRC=drivers/power/power.c ;;
        i2c) SRC=hal/common/i2c_bitbang.c ;;
    esac
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -DM5OS_BOARD_TAB5=1 -Ikernel/include "tools/hardware/${TEST}_test.c" "$SRC" -o "$OUT/$TEST"
    "$OUT/$TEST"
done
# Compile new diagnostics on the other boards too; Tab5-only calls must not leak into their builds.
for BOARD in CORES3 CARDPUTER; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D"M5OS_BOARD_$BOARD"=1 -Ikernel/include \
        -fsyntax-only drivers/power/power.c user/shell/hardware.c hal/common/i2c_bitbang.c
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -D"M5OS_BOARD_$BOARD"=1 -Ikernel/include \
        tools/hardware/i2c_test.c hal/common/i2c_bitbang.c -o "$OUT/i2c_$BOARD"
    "$OUT/i2c_$BOARD"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/hardware/board_test.c \
    hal/esp32p4/board_tab5.c -o "$OUT/board"
"$OUT/board"
