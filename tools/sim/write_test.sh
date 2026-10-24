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
keys=[(44,1)]+[(ord(c)-97+4,0) for c in 'nihongo']+[(44,0),(44,0),(30,0),(40,0),(44,1),(22,1)]
a=['shot 100 initial']+[f'key 70 {k} {m} 0' for k,m in keys[:10]]
a+=['shot 100 candidates']+[f'key 70 {k} {m} 0' for k,m in keys[10:]]
a+=['shot 100 saved','key 70 20 1 0']
(r/'keys.txt').write_text('\n'.join(a)+'\n')
(r/'expected.txt').write_text('日本語'+text)
PY
binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$binary" --write --headless --workspace "$result/work" --screenshots "$result/shots" --source /home/note.txt --replay "$result/keys.txt"
cmp "$result/expected.txt" "$result/work/home/note.txt"
printf 'Writing editor UTF-8 save / IME replay PASS: %s\n' "$result"
