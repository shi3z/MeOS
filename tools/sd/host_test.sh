#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-sd.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Ikernel/include tools/sd/spi_test.c drivers/sd/sd_tab5.c -o "$OUT/spi_test"
"$OUT/spi_test"
python3 tools/sd/make_images.py "$OUT"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include -Ithird_party/fatfs \
    tools/sd/fat_test.c fs/fat/fat_ro.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c -o "$OUT/fat_test"
for IMAGE in "$OUT"/*.img; do "$OUT/fat_test" "$IMAGE"; done
# Write and format APIs must not even be present in the linked binary.
nm "$OUT/fat_test" > "$OUT/symbols"
if grep -E ' [Tt] _?(f_write|f_mkfs|f_unlink|f_mkdir|f_truncate)$' "$OUT/symbols"; then
    echo 'unexpected writable FatFs API' >&2
    exit 1
fi
