#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-quake.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
printf '#pragma once\n#define EXT_RAM_BSS_ATTR\n' > "$OUT/esp_attr.h"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -I"$OUT" -Ikernel/include \
    tools/quake/host_test.c apps/quake/libc.c kernel/lib/number.c -lm -o "$OUT/test"
"$OUT/test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -I"$OUT" -Ikernel/include \
    tools/quake/receive_test.c apps/quake/receive.c kernel/lib/crc32.c -o "$OUT/receive-test"
"$OUT/receive-test"
python3 tools/quake/stack_check_test.py
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -Ikernel/include \
    tools/quake/input_test.c apps/quake/input.c kernel/dev/keyboard_raw.c drivers/usbkbd/hid_keys.c -o "$OUT/input-test"
"$OUT/input-test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/keyboard/tab5_mode_test.c kernel/dev/keyboard_raw.c drivers/usbkbd/hid_keys.c -o "$OUT/keyboard-mode-test"
"$OUT/keyboard-mode-test"

python3 tools/quake/upload_test.py

cc -std=c11 -Wall -Wextra -Werror -O3 -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/quake/delta_test.c -o "$OUT/delta-test"
"$OUT/delta-test"

cc -std=c11 -Wall -Wextra -Werror -O3 -g -fsanitize=address,undefined tools/quake/math_test.c -lm -o "$OUT/math-test"
"$OUT/math-test"

cc -std=c11 -Wall -Wextra -Werror -O3 -g -fsanitize=address,undefined tools/quake/audio_test.c -o "$OUT/audio-test"
"$OUT/audio-test"

cc -std=c11 -Wall -Wextra -Werror -O2 -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -Ikernel/include tools/quake/profile_test.c apps/quake/profile.c -o "$OUT/profile-test"
"$OUT/profile-test"
cc -std=c11 -Wall -Wextra -Werror -O2 -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -DMEOS_QUAKE_DETAIL_PROFILE=1 -Ikernel/include tools/quake/profile_test.c apps/quake/profile.c -o "$OUT/detail-profile-test"
"$OUT/detail-profile-test"

cc -std=c11 -Wall -Wextra -Werror -O3 -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE_HIGHRES=1 -Ikernel/include tools/quake/hires_test.c -o "$OUT/hires-test"
"$OUT/hires-test"

cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -DM5OS_QUAKE_HIGHRES=1 -Ikernel/include tools/quake/input_test.c apps/quake/input.c kernel/dev/keyboard_raw.c drivers/usbkbd/hid_keys.c -o "$OUT/hires-input-test"
"$OUT/hires-input-test"

# Exercise the production descriptor submission/pipeline with delayed hardware.
for RES in 0 1; do
    cc -std=c11 -Wall -Wextra -Werror -O3 -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -DM5OS_QUAKE=1 -DM5OS_QUAKE_HIGHRES=$RES -Itools/quake/dma_mock -Ikernel/include tools/quake/dma_batch_test.c -o "$OUT/dma-batch-$RES"
    "$OUT/dma-batch-$RES"
done
