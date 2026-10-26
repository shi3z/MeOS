#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-player.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -g -fsanitize=address,undefined \
    -Ikernel/include tools/player/stream_test.c user/player/mp3_stream.c -o "$OUT/stream" -lm
for FILE in tools/player/fixtures/mono-*.mp3; do
    RATE=$(basename "$FILE" .mp3 | cut -d- -f2)
    "$OUT/stream" "$FILE" "$OUT/$(basename "$FILE").pcm" "$RATE" 1
done
"$OUT/stream" tools/player/fixtures/stereo-44100.mp3 "$OUT/stereo-44100.mp3.pcm" 44100 2
python3 tools/player/check_pcm.py "$OUT"

for TEST in lifecycle vfs; do
    case "$TEST" in
        lifecycle) SOURCE=user/player/mp3_player.c ;;
        vfs) SOURCE='fs/vfs/vfs.c fs/vfs/write_file.c' ;;
    esac
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -DM5OS_BOARD_TAB5=1 -Ikernel/include "tools/player/${TEST}_test.c" $SOURCE -o "$OUT/$TEST"
    "$OUT/$TEST"
done
