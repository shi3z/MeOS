#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-doom.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -DM5OS_DOOM=1 -Ikernel/include \
    tools/doom/libc_test.c apps/doom/libc.c -o "$OUT/libc_test"
"$OUT/libc_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -DM5OS_DOOM=1 -Ikernel/include -Ithird_party/doomgeneric \
    tools/doom/input_test.c apps/doom/input.c -o "$OUT/input_test"
"$OUT/input_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -DM5OS_DOOM=1 -Ikernel/include \
    tools/doom/mixer_test.c apps/doom/mixer.c -o "$OUT/mixer_test"
"$OUT/mixer_test" "$@"
cc -std=c11 -Wall -Wextra -Werror -O3 -funroll-loops -g -fsanitize=address,undefined \
    -DM5OS_BOARD_TAB5=1 -Ikernel/include tools/doom/resize_test.c -o "$OUT/resize_test"
"$OUT/resize_test"
if [ $# -gt 0 ]; then
    # Keep the upstream engine's historical arithmetic out of UBSan; test the port separately above.
    python3 tools/doom/pack_wad.py "$1" "$OUT/wad.z"
    SOURCES=$(sed -n 's|.*${DOOM_ROOT}/\(.*\.c\)$|third_party/doomgeneric/\1|p' components/doomengine/sources.cmake)
    cc -O2 -w -DDOOM_COMPAT_DECLARATIONS_ONLY -Ikernel/include -Ithird_party/doomgeneric \
        -c tools/doom/headless.c -o "$OUT/headless.o"
    cc -O2 -w -DM5OS_BOARD_TAB5=1 -DM5OS_DOOM=1 -Ikernel/include \
        -c apps/doom/libc.c -o "$OUT/libc.o"
    cc -O2 -w -fno-builtin -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
        -DI_FinishUpdate=doom_engine_finish_update -DI_SetPalette=doom_engine_set_palette \
        -include components/doomengine/compat.h -Ikernel/include -Ithird_party/doomgeneric \
        -c third_party/doomgeneric/i_video.c -o "$OUT/video_engine.o"
    cc -O2 -w -DM5OS_BOARD_TAB5=1 -DM5OS_DOOM=1 -Ikernel/include -Ithird_party/doomgeneric \
        -c apps/doom/video.c -o "$OUT/video_port.o"
    SOURCES=$(printf '%s\n' "$SOURCES" | sed '/\/i_video.c$/d')
    cc -O2 -w -fno-builtin -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
        -include components/doomengine/compat.h -Ikernel/include -Ithird_party/doomgeneric \
        $SOURCES third_party/doomgeneric/i_sound.c components/doomengine/i_system_meos.c "$OUT/headless.o" "$OUT/libc.o" "$OUT/video_engine.o" "$OUT/video_port.o" -lm -o "$OUT/headless"
    "$OUT/headless" "$1" "${2:-/tmp/meos-doom-headless.ppm}"
fi
