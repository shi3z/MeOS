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
# Formatting stays excluded; read-only mount tests still reject all writes.
nm "$OUT/fat_test" > "$OUT/symbols"
if grep -E ' [Tt] _?(f_mkfs|f_fdisk)$' "$OUT/symbols"; then
    echo 'unexpected formatting FatFs API' >&2
    exit 1
fi

${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include -Ithird_party/fatfs tools/sd/fat_write_test.c \
    fs/fat/fat_ro.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c -o "$OUT/fat_write_test"
for IMAGE in "$OUT"/*.img; do
    case "$IMAGE" in *cycle*) continue;; esac
    "$OUT/fat_write_test" "$IMAGE"
done

${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include -Ithird_party/fatfs tools/sd/vfat_write_vfs_test.c user/shell/sdbench.c \
    fs/vfs/vfs.c fs/fat/fat_ro.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c \
    -o "$OUT/vfat_write_vfs_test"
"$OUT/vfat_write_vfs_test" "$OUT/fat32-mbr.img"
