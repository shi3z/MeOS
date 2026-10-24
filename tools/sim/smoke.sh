#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
sim_results=$(mktemp -d "${TMPDIR:-/tmp}/meos-sim-smoke.XXXXXX")
mkdir "$sim_results/workspace" "$sim_results/screenshots"
M5OS_SIM_SANITIZE=1 sh tools/sim/run.sh --headless --smoke \
    --workspace "$sim_results/workspace" --screenshots "$sim_results/screenshots"
echo "Smoke artifacts: $sim_results"
