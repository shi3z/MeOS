#!/bin/sh
# ESP-IDF export.sh requires Bash or Zsh to locate its installation.
if [ -z "${BASH_VERSION:-}" ] && [ -z "${ZSH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi
# Build for a board:   tools/build.sh [cores3|cardputer|tab5] [idf.py args...]     (default: cores3)
#   tools/build.sh cardputer flash -p /dev/cu.usbmodemXXXX
# Each board has its own build directory and sdkconfig, so they never mix.
set -e
cd "$(dirname "$0")/.."
BOARD=${1:-cores3}
case "$BOARD" in cores3|cardputer|tab5) shift ;; *) BOARD=cores3 ;; esac
export M5OS_BOARD=$BOARD                      # must come first: env.sh picks the ESP-IDF from it
. tools/env.sh
if [ "$BOARD" = cores3 ]; then
    exec idf.py "${@:-build}"                  # the original layout: build/ and sdkconfig
fi
[ $# -eq 0 ] && set -- build
TARGET=esp32s3; [ "$BOARD" = tab5 ] && TARGET=esp32p4
exec idf.py -B build-$BOARD -DSDKCONFIG=sdkconfig.$BOARD -DIDF_TARGET=$TARGET "$@"
