#!/bin/sh
# Builds the Japanese input method's dictionary from an SKK dictionary and uploads
# it as the SD bulk asset /assets/ime-dict.bin.  The firmware loads it into PSRAM
# after the persistent SD root is mounted; recovery boots use the built-in mini dictionary.
#
#   tools/ime_flash_dict.sh -p /dev/cu.usbmodemXXXX            downloads SKK-JISYO.L and uploads it
#   tools/ime_flash_dict.sh -p PORT path/to/SKK-JISYO.M ...    use your own SKK file(s)
#   tools/ime_flash_dict.sh --build-only [files...]            only write tools/ime/cache/dict.bin
#
# SKK-JISYO.L is licensed GPL-2.0-or-later (skk-dev/dict); keep that in mind before redistributing the flashed image.
set -e
cd "$(dirname "$0")/.."
PORT=; BUILD_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        -p) PORT=$2; shift 2 ;;
        --build-only) BUILD_ONLY=1; shift ;;
        *) break ;;
    esac
done
CACHE=tools/ime/cache
mkdir -p $CACHE
if [ $# -eq 0 ]; then
    [ -f $CACHE/SKK-JISYO.L ] || curl -fL -o $CACHE/SKK-JISYO.L https://raw.githubusercontent.com/skk-dev/dict/master/SKK-JISYO.L
    set -- $CACHE/SKK-JISYO.L
fi
python3 tools/ime_mkdict.py -o $CACHE/dict.bin "$@"
[ $BUILD_ONLY = 1 ] && exit 0
[ -n "$PORT" ] || { echo "give the serial port with -p (e.g. -p /dev/cu.usbmodem1101)"; exit 1; }
export M5OS_BOARD=tab5
. tools/env.sh                         # selects the Python environment that provides pyserial
python3 tools/push_file.py --port "$PORT" $CACHE/dict.bin /assets/ime-dict.bin
