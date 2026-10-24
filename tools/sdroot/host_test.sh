#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-sdroot.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -DLFS_THREADSAFE -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT \
    -Ikernel/include tools/sdroot/adapter_test.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
    fs/cow/cow.c fs/cow/store.c fs/cow/blockdev.c fs/m5fs/m5fs.c kernel/lib/crc32.c -o "$OUT/adapter"
"$OUT/adapter"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/sdroot/guards_test.c user/shell/sd.c -o "$OUT/guards"
"$OUT/guards"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/sdroot/cache_test.c kernel/app/wasm_cache.c -o "$OUT/cache"
"$OUT/cache"
for BOARD in TAB5 CORES3; do
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
        -DM5OS_BOARD_$BOARD=1 -Ikernel/include tools/sdroot/boot_test.c kernel/boot/rootfs.c -o "$OUT/boot_$BOARD"
    "$OUT/boot_$BOARD"
done
