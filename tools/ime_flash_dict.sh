#!/bin/sh
# Builds the Japanese input method's dictionary from an SKK dictionary and writes it to the Tab5's flash
# (region "imedict" at 0x400000, see partitions_tab5.csv).  Needed once; the firmware loads it at every boot.
#
#   tools/ime_flash_dict.sh -p /dev/cu.usbmodemXXXX            downloads SKK-JISYO.L and flashes it
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
. tools/env.sh
python3 -m esptool --chip esp32p4 -p "$PORT" write_flash 0x400000 $CACHE/dict.bin
