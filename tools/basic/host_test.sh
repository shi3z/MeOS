#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-basic.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow \
    tools/basic/host_test.c user/basic/basic.c third_party/ubasic/ubasic.c \
    third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c -DM5OS_BOARD_TAB5=1 -Ikernel/include -lm -o "$OUT/test"
"$OUT/test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/file_test.c user/basic/file.c user/basic/basic.c \
    third_party/ubasic/ubasic.c third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c -DM5OS_BOARD_TAB5=1 -Ikernel/include -lm -o "$OUT/file_test"
"$OUT/file_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/editor_test.c user/edit/edit.c -lm -o "$OUT/editor_test"
"$OUT/editor_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow \
    tools/basic/input_test.c user/basic/basic.c user/basic/console.c \
    third_party/ubasic/ubasic.c third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c -DM5OS_BOARD_TAB5=1 -Ikernel/include -lm -o "$OUT/input_test"
"$OUT/input_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow \
    -DM5OS_BOARD_TAB5=1 -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/graphics_test.c user/basic/graphics.c user/basic/basic.c \
    drivers/lcd/display_claim.c third_party/ubasic/ubasic.c \
    third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c -DM5OS_BOARD_TAB5=1 -Ikernel/include -lm -o "$OUT/graphics_test"
"$OUT/graphics_test"
for BOARD in CORES3 CARDPUTER; do
    cc -std=c11 -Wall -Wextra -Werror -D"M5OS_BOARD_$BOARD"=1 -Ikernel/include \
        -fsyntax-only user/basic/graphics.c drivers/lcd/display_claim.c \
        fs/programs/store.c fs/programs/vfs.c user/basic/programs_tab5.c
done
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/number_test.c kernel/lib/number.c kernel/lib/number_math.c -lm -o "$OUT/number_test"
"$OUT/number_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/floating_test.c user/basic/basic.c third_party/ubasic/ubasic.c \
    third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c -lm -o "$OUT/floating_test"
"$OUT/floating_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/math_test.c user/basic/basic.c third_party/ubasic/ubasic.c \
    third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c \
    -o "$OUT/math_test" -lm
"$OUT/math_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/step_test.c user/basic/basic.c third_party/ubasic/ubasic.c \
    third_party/ubasic/tokenizer.c kernel/lib/number.c kernel/lib/number_math.c \
    -o "$OUT/step_test" -lm
"$OUT/step_test"
python3 tools/basic/render_test.py
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/store_test.c fs/programs/store.c kernel/lib/crc32.c -o "$OUT/store_test"
"$OUT/store_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined,float-cast-overflow -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    tools/basic/program_fs_test.c fs/programs/store.c fs/programs/vfs.c kernel/lib/crc32.c \
    user/basic/file.c user/basic/basic.c third_party/ubasic/ubasic.c third_party/ubasic/tokenizer.c \
    kernel/lib/number.c kernel/lib/number_math.c -lm -o "$OUT/program_fs_test"
"$OUT/program_fs_test"
python3 tools/basic/config_test.py
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -DM5OS_BOARD_TAB5=1 \
    -Itools/basic/rom_stubs -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/basic/program_rom_test.c user/basic/programs_tab5.c \
    fs/programs/store.c fs/programs/vfs.c kernel/lib/crc32.c -o "$OUT/program_rom_test"
"$OUT/program_rom_test"
