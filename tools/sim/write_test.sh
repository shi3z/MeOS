#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
result=$(mktemp -d "${TMPDIR:-/tmp}/meos-write.XXXXXX")
mkdir "$result/work" "$result/work/home" "$result/shots"
python3 - "$result" <<'PY'
import pathlib,sys
r=pathlib.Path(sys.argv[1])
text='日本語と English を、無理なく読める大きさで。\n文章を書く道具として、余白と行間も大切にしたい。\n'+('長い文章も画面の幅に合わせて自然に折り返します。'*5)+'\n'
(r/'work/home/note.txt').write_text(text)
# Toggle MeOS IME, type nihongo, open candidates, select first candidate, commit.
conversion=[(ord(c)-97+4,0) for c in 'nihongo']+[(44,0),(44,0),(30,0),(40,0)]
# Undo must remove the first confirmed conversion as one edit. Confirm it
# again; a byte/codepoint-at-a-time path leaves a visible prefix and fails cmp.
keys=[(44,1)]+conversion+[(29,1)]+conversion+[(44,1),(22,1)]
a=['shot 100 initial']+[f'key 70 {k} {m} 0' for k,m in keys[:10]]
a+=['shot 100 candidates']+[f'key 70 {k} {m} 0' for k,m in keys[10:]]
a+=['shot 100 saved','key 70 23 1 0','shot 100 horizontal','key 70 20 1 0']
(r/'keys.txt').write_text('\n'.join(a)+'\n')
(r/'expected.txt').write_text('日本語'+text)
PY
binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$binary" --write --headless --workspace "$result/work" --screenshots "$result/shots" --source /home/note.txt --replay "$result/keys.txt"
cmp "$result/expected.txt" "$result/work/home/note.txt"
for shot in initial candidates saved horizontal; do test -s "$result/shots/$shot.bmp"; done
cmp -s "$result/shots/initial.bmp" "$result/shots/candidates.bmp" && {
    echo "Writing editor IME candidate frame did not change" >&2
    exit 1
}
cmp -s "$result/shots/saved.bmp" "$result/shots/horizontal.bmp" && {
    echo "Writing editor vertical/horizontal toggle did not change the frame" >&2
    exit 1
}
printf 'Writing editor UTF-8 save / IME replay PASS: %s\n' "$result"
