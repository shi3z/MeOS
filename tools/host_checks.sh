#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
for SUITE in foundations hardware touch stack peripheral microphone psram lcd ime sd player cow sdroot atomic-save; do
    echo "Host suite: $SUITE"
    sh "tools/$SUITE/host_test.sh"
done
