#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
result=$(mktemp -d "${TMPDIR:-/tmp}/meos-scheme-ui.XXXXXX")
mkdir "$result/work" "$result/work/home" "$result/shots"
: >"$result/work/home/eval.scm"
cat >"$result/keys.txt" <<'EOF'
shot 100 initial
# F2 opens the BIZ UD minibuffer. Enter (sqrt 81), then capture the REPL result.
key 60 59 0 0
shot 100 minibuffer
key 40 38 2 0
key 0 22 0 0
key 0 20 0 0
key 0 21 0 0
key 0 23 0 0
key 0 44 0 0
key 0 37 0 0
key 0 30 0 0
key 0 39 2 0
key 0 40 0 0
shot 100 result
key 40 20 1 0
EOF
binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$binary" --headless --workspace "$result/work" --screenshots "$result/shots" \
    --source /home/eval.scm --replay "$result/keys.txt"
for shot in initial minibuffer result; do test -s "$result/shots/$shot.bmp"; done
cmp -s "$result/shots/initial.bmp" "$result/shots/minibuffer.bmp" && {
    echo "Scheme BIZ UD minibuffer did not appear" >&2
    exit 1
}
cmp -s "$result/shots/minibuffer.bmp" "$result/shots/result.bmp" && {
    echo "Scheme REPL result frame did not change" >&2
    exit 1
}
printf 'Scheme BIZ UD edit/minibuffer/evaluation replay PASS: %s\n' "$result"
