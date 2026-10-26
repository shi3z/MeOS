#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
sim_result=$(mktemp -d "${TMPDIR:-/tmp}/meos-replay.XXXXXX")
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    tools/sim/replay.c tools/sim/replay_test.c -o "$sim_result/parser-test"
"$sim_result/parser-test"
mkdir "$sim_result/record" "$sim_result/replay" "$sim_result/images-a" "$sim_result/images-b"
sim_binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$sim_binary" --headless --smoke \
    --workspace "$sim_result/record" --screenshots "$sim_result/images-a" \
    --record "$sim_result/session.keys"
"$sim_binary" --headless --workspace "$sim_result/replay" \
    --screenshots "$sim_result/images-b" --replay "$sim_result/session.keys"
cmp "$sim_result/record/home/workspace.scm" "$sim_result/replay/home/workspace.scm"
for name in initial graph ime-candidates final; do
    cmp "$sim_result/images-a/$name.bmp" "$sim_result/images-b/$name.bmp"
done
cmp -s "$sim_result/images-a/graph.bmp" "$sim_result/images-a/ime-candidates.bmp" && {
    echo 'Scheme IME candidate frame did not change' >&2
    exit 1
}
echo "Replay PASS: saved source and all four screenshots match byte for byte"
mkdir "$sim_result/caps" "$sim_result/images-c"
"$sim_binary" --headless --workspace "$sim_result/caps" \
    --screenshots "$sim_result/images-c" --replay tools/sim/scenarios/editor.keys
test "$(head -n 1 "$sim_result/caps/home/workspace.scm")" = '; ABC'
test -s "$(find "$sim_result/images-c" -name 'capture-*.bmp' -print -quit)"
echo "Replay PASS: Caps Lock, Shift, right Ctrl and F12 capture"
# Startup errors must fail, including when the app cannot open its source.
mkdir "$sim_result/failure" "$sim_result/failure/home"
printf '\300' > "$sim_result/failure/home/invalid.scm"
if "$sim_binary" --headless --workspace "$sim_result/failure" --source /home/invalid.scm > "$sim_result/startup-error.log" 2>&1; then
    echo 'Startup failure incorrectly returned success' >&2; exit 1
fi
echo "Startup failure exit-code PASS"
echo "Replay artifacts: $sim_result"
