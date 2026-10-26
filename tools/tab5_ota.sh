#!/bin/sh
# Inspect and operate the Tab5 A/B application slots from a development host.
# This intentionally does not combine write + activation: verify a completed
# write before changing otadata, and keep the factory image as recovery media.
set -eu
cd "$(dirname "$0")/.."

usage() {
    cat >&2 <<'EOF'
usage: tools/tab5_ota.sh -p PORT status
       tools/tab5_ota.sh -p PORT backup OUTPUT.bin
       tools/tab5_ota.sh -p PORT install SLOT [IMAGE.bin]
       tools/tab5_ota.sh -p PORT activate SLOT
       tools/tab5_ota.sh -p PORT factory

SLOT is 0 or 1. IMAGE defaults to build-tab5/m5os.bin. `install` does not
activate or reset. `factory` erases only otadata so the immutable factory image
is selected at the next reset.
EOF
    exit 2
}

[ "${1:-}" = -p ] && [ $# -ge 3 ] || usage
PORT=$2
ACTION=$3
shift 3
export M5OS_BOARD=tab5
. tools/env.sh
OTATOOL=$IDF_PATH/components/app_update/otatool.py

ota() {
    python3 "$OTATOOL" -p "$PORT" "$@"
}

case "$ACTION" in
    status)
        [ $# -eq 0 ] || usage
        ota read_otadata
        ;;
    backup)
        [ $# -eq 1 ] || usage
        [ ! -e "$1" ] || { echo "refusing to overwrite: $1" >&2; exit 1; }
        python3 -m esptool --chip esp32p4 -p "$PORT" read_flash 0 0x1000000 "$1"
        shasum -a 256 "$1"
        ;;
    install)
        [ $# -ge 1 ] && [ $# -le 2 ] || usage
        SLOT=$1
        IMAGE=${2:-build-tab5/m5os.bin}
        case "$SLOT" in 0|1) ;; *) usage ;; esac
        [ -f "$IMAGE" ] || { echo "image not found: $IMAGE" >&2; exit 1; }
        BYTES=$(wc -c < "$IMAGE" | tr -d ' ')
        [ "$BYTES" -le $((0x3e0000)) ] || { echo "image exceeds 0x3e0000-byte slot" >&2; exit 1; }
        ota write_ota_partition --slot "$SLOT" --input "$IMAGE"
        echo "OTA slot $SLOT written and verified; it is not active."
        ;;
    activate)
        [ $# -eq 1 ] || usage
        case "$1" in 0|1) ;; *) usage ;; esac
        ota switch_ota_partition --slot "$1"
        echo "OTA slot $1 selected; reset the Tab5 to boot it."
        ;;
    factory)
        [ $# -eq 0 ] || usage
        ota erase_otadata
        echo "Factory recovery selected; reset the Tab5 to boot it."
        ;;
    *) usage ;;
esac
