#!/bin/sh
if [ -z "${BASH_VERSION:-}" ] && [ -z "${ZSH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi
set -eu
cd "$(dirname "$0")/../.."
export M5OS_BOARD=tab5 M5OS_QUAKE=1
unset M5OS_DOOM M5OS_DOOM_RELEASE
. tools/env.sh
[ $# -gt 0 ] || set -- build
exec idf.py -B build-quake -DSDKCONFIG=sdkconfig.quake -DIDF_TARGET=esp32p4 "$@"
