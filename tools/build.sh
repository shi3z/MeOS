#!/bin/sh
# Build for a board:   tools/build.sh [cores3|tab5] [idf.py args...]     (default: cores3)
#   tools/build.sh tab5 flash -p /dev/cu.usbmodemXXXX
# Each board has its own build directory and sdkconfig, so they never mix.
set -e
cd "$(dirname "$0")/.."
BOARD=${1:-cores3}
case "$BOARD" in cores3|tab5) shift ;; *) BOARD=cores3 ;; esac
export M5OS_BOARD=$BOARD                      # must come first: env.sh picks the ESP-IDF from it
. tools/env.sh
if [ "$BOARD" = cores3 ]; then
    exec idf.py "${@:-build}"                  # the original layout: build/ and sdkconfig
fi
[ $# -eq 0 ] && set -- build
TARGET=esp32p4
exec idf.py -B build-$BOARD -DSDKCONFIG=sdkconfig.$BOARD -DIDF_TARGET=$TARGET "$@"
