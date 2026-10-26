#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d "${TMPDIR:-/tmp}/meos-foundations.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
for TEST in crash pi cleanup app time health; do
    case "$TEST" in
        crash) SRC=kernel/lib/crash_record.c ;;
        pi) SRC=kernel/ipc/mutex.c ;;
        cleanup) SRC=kernel/app/cleanup.c ;;
        app) SRC='kernel/app/app.c kernel/app/cleanup.c' ;;
        time) SRC='kernel/lib/utc.c kernel/lib/wall_time.c drivers/rtc/rtc_tab5.c' ;;
        health) SRC=kernel/lib/health.c ;;
    esac
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
        -DM5OS_BOARD_TAB5=1 -Ikernel/include "tools/foundations/${TEST}_test.c" $SRC -o "$OUT/$TEST"
    "$OUT/$TEST"
done
sh tools/runtime/host_test.sh
