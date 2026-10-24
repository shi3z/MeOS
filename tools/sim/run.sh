#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
sim_binary=$(sh tools/sim/build.sh)
if [ "$#" -eq 0 ]; then
    sim_workspace=$(mktemp -d "${TMPDIR:-/tmp}/meos-sim.XXXXXX")
    mkdir "$sim_workspace/screenshots"
    set -- --workspace "$sim_workspace" --screenshots "$sim_workspace/screenshots"
fi
exec "$sim_binary" "$@"
