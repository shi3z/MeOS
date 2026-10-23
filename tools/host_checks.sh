#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
for SUITE in foundations hardware touch stack peripheral microphone psram lcd ime; do
    echo "Host suite: $SUITE"
    sh "tools/$SUITE/host_test.sh"
done
