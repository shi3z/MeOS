#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
sim_candidate=$(mktemp -d "${TMPDIR:-/tmp}/meos-clean-source.XXXXXX")
python3 tools/sim/source_bundle.py --output "$sim_candidate/source"
# Build from a fresh clone of an explicit candidate, not the live dirty tree.
# The temporary index/config belongs to this local candidate, never the user's repo.
git -C "$sim_candidate/source" init -q
git -C "$sim_candidate/source" add .
git -C "$sim_candidate/source" -c user.name='MeOS source check' \
    -c user.email='source-check@localhost' -c commit.gpgsign=false commit -qm 'Local simulator source candidate'
git clone -q --no-local "$sim_candidate/source" "$sim_candidate/clone"
(
    cd "$sim_candidate/clone"
    sh tools/scheme/host_test.sh
    sh tools/sim/replay_test.sh
    M5OS_SIM_EVAL_POLL_DELAY_MS=2 sh tools/sim/replay_test.sh
    sh tools/sim/write_test.sh
    sh tools/sim/handoff_test.sh
)
echo "Clean-source clone PASS: $sim_candidate/clone"
