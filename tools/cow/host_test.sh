#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-cow.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    tools/cow/test.c fs/cow/cow.c -o "$OUT/test"
"$OUT/test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/cow/m5fs_test.c fs/vfs/vfs.c fs/vfs/m5fs_vfs.c fs/vfs/write_file.c fs/cow/cow.c fs/cow/blockdev.c \
    fs/m5fs/m5fs.c kernel/lib/crc32.c -o "$OUT/m5fs_test"
"$OUT/m5fs_test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    tools/cow/store_test.c fs/cow/cow.c fs/cow/store.c -o "$OUT/store_test"
"$OUT/store_test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Ikernel/include tools/cow/blockdev_test.c fs/cow/cow.c fs/cow/blockdev.c -o "$OUT/blockdev_test"
"$OUT/blockdev_test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/cow/atomic_test.c fs/vfs/vfs.c fs/vfs/m5fs_vfs.c fs/vfs/write_file.c fs/cow/cow.c fs/cow/store.c \
    fs/cow/blockdev.c fs/m5fs/m5fs.c fs/m5fs/m5fs_test.c \
    drivers/ramdisk/ramdisk.c kernel/lib/crc32.c -o "$OUT/atomic_test"
"$OUT/atomic_test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Ikernel/include tools/cow/mount_test.c fs/m5fs/m5fs.c kernel/lib/crc32.c -o "$OUT/mount_test"
"$OUT/mount_test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -DLFS_THREADSAFE -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT \
    -Ikernel/include tools/cow/sdroot_test.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
    fs/cow/cow.c fs/cow/store.c fs/cow/blockdev.c fs/m5fs/m5fs.c kernel/lib/crc32.c -o "$OUT/sdroot_test"
"$OUT/sdroot_test"
