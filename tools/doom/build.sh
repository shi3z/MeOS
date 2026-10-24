#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
export M5OS_BOARD=tab5 M5OS_DOOM=1
BUILD_DIR=${M5OS_DOOM_BUILD_DIR:-build-doom}
SDKCONFIG=sdkconfig.doom
EMBED=ON
if [ "${1:-}" = --without-wad ]; then
    shift
    export M5OS_DOOM_RELEASE=1
    BUILD_DIR=${M5OS_DOOM_BUILD_DIR:-build-doom-release}
    SDKCONFIG=sdkconfig.doom-release
    EMBED=OFF
    REV=$(git rev-parse HEAD 2>/dev/null || cat SOURCE_REVISION 2>/dev/null || printf unknown)
    VER=$(printf '%.12s' "$REV")
    [ $# -gt 0 ] || set -- build
    set -- "-DPROJECT_VER=$VER" "$@"
    SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD 2>/dev/null || cat SOURCE_DATE_EPOCH 2>/dev/null || printf 0)}
    export SOURCE_DATE_EPOCH
else
    unset M5OS_DOOM_RELEASE
fi
. tools/env.sh
[ $# -gt 0 ] || set -- build
exec idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -DIDF_TARGET=esp32p4 \
    -DM5OS_DOOM_EMBED_WAD="$EMBED" "$@"
