#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-microphone.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Ikernel/include tools/microphone/host_test.c drivers/audio/mic_levels.c \
    drivers/audio/mic_codec.c -o "$OUT/test"
"$OUT/test"
