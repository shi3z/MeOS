#!/bin/sh
# ESP-IDF export.sh requires Bash or Zsh to locate its installation.
if [ -z "${BASH_VERSION:-}" ] && [ -z "${ZSH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi
set -eu
cd "$(dirname "$0")/../.."
export M5OS_BOARD=tab5
. tools/env.sh
python3 tools/doom/release.py check-dependencies --idf "$IDF_PATH"
export SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
# Explicitly reconfigure so the metadata records the source revision being built.
sh tools/doom/build.sh --without-wad reconfigure build
python3 tools/doom/release.py verify --report "${M5OS_DOOM_BUILD_DIR:-build-doom-release}/verification.json" \
    --build-dir "${M5OS_DOOM_BUILD_DIR:-build-doom-release}"
