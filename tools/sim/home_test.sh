#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
result=$(mktemp -d "${TMPDIR:-/tmp}/meos-home.XXXXXX")
mkdir "$result/work" "$result/shots"
cat >"$result/home.keys" <<'EOF'
# Capture Home, move Write -> Scheme -> Camera, then return to Shell.
shot 100 initial
key 50 79 0 0
shot 50 scheme
key 50 81 0 0
shot 50 camera
key 50 20 0 0
EOF
binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$binary" --home --headless --workspace "$result/work" --screenshots "$result/shots" \
    --replay "$result/home.keys"
test -s "$result/shots/initial.bmp"
test -s "$result/shots/scheme.bmp"
test -s "$result/shots/camera.bmp"
cmp -s "$result/shots/initial.bmp" "$result/shots/scheme.bmp" && {
    echo "Home cursor did not move" >&2
    exit 1
}
printf 'Home keyboard/render replay PASS: %s\n' "$result"
