#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-wifi.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    tools/wifi/rx_test.c drivers/wifi/sdio_rx.c -o "$OUT/rx"
"$OUT/rx"
