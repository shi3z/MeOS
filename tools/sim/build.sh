#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
sim_build_root=${M5OS_SIM_BUILD_ROOT:-$PWD/build-host-sim}
mkdir -p "$sim_build_root"
sim_build=$(mktemp -d "$sim_build_root/session.XXXXXX")
sim_sdl_config=${SDL2_CONFIG:-sdl2-config}
command -v "$sim_sdl_config" >/dev/null 2>&1 || { echo 'SDL2 development files required (macOS: brew install sdl2; Debian/Ubuntu: libsdl2-dev)' >&2; exit 1; }
sim_sanitize=""
if [ "${M5OS_SIM_SANITIZE:-0}" = 1 ]; then sim_sanitize="-fsanitize=undefined -fno-sanitize-recover=all"; fi
# Compiler/config output contains flags; splitting is intentional. No eval.
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -O1 $sim_sanitize -DM5OS_BOARD_TAB5=1 -Ikernel/include \
    $("$sim_sdl_config" --cflags) tools/sim/native.c tools/sim/replay.c user/scheme/ide.c user/scheme/scheme.c \
    user/scheme/buffer.c user/write/write.c drivers/lcd/bizud.c user/tui/dqwin.c kernel/lib/number.c \
    drivers/lcd/font8x8.c drivers/lcd/font_ja16.c drivers/usbkbd/hid_keys.c \
    drivers/ime/ime.c drivers/ime/conv.c drivers/ime/romaji.c drivers/ime/ime_dict.c \
    drivers/ime/ime_mini_dict.c -o "$sim_build/meos-sim" $("$sim_sdl_config" --libs) -lm
printf '%s\n' "$sim_build/meos-sim"
