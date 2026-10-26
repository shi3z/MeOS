#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
handoff_result=$(mktemp -d "${TMPDIR:-/tmp}/meos-handoff.XXXXXX")
mkdir "$handoff_result/work" "$handoff_result/work/home" "$handoff_result/shots"
python3 - "$handoff_result" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
source='(define (wave x) (* (sin x) (exp (* -0.1 x))))\n(plot wave 0 30 "wave")\n'
note='Initial text\n'
(r/'work/home/handoff.scm').write_text(source)
(r/'work/home/handoff.txt').write_text(note)
(r/'work/home/invalid.scm').write_bytes(b'\xc0')
for n in range(6):
    scheme=n%2==0
    keys=[]
    if scheme:keys += [(27,1),(5,1)]  # C-x C-b: evaluate and draw graph
    if scheme:keys += [(51,0),(44,0)] # Scheme comment prefix
    keys += [(44,1)]                 # real MeOS IME on
    keys += [(ord(c)-97+4,0) for c in 'nihongo']
    keys += [(44,0),(44,0),(30,0),(40,0),(44,1)] # choose/commit/off
    if scheme:keys += [(40,0)]
    keys += [(22,1)]                 # save
    actions=[f'key 35 {key} {mod} 0' for key,mod in keys]
    actions += [f'shot 50 stage-{n+1}', 'key 35 20 1 0'] # C-q
    (r/f'{n}.keys').write_text('\n'.join(actions)+'\n')
(r/'expected.scm').write_text('; 日本語\n'*3+source)
(r/'expected.txt').write_text('日本語'*3+note)
PY
handoff_binary=$(M5OS_SIM_SANITIZE=1 sh tools/sim/build.sh)
"$handoff_binary" --headless --workspace "$handoff_result/work" --screenshots "$handoff_result/shots" \
    --stage scheme /home/handoff.scm "$handoff_result/0.keys" \
    --stage write /home/handoff.txt "$handoff_result/1.keys" \
    --stage scheme /home/handoff.scm "$handoff_result/2.keys" \
    --stage write /home/handoff.txt "$handoff_result/3.keys" \
    --stage scheme /home/handoff.scm "$handoff_result/4.keys" \
    --stage write /home/handoff.txt "$handoff_result/5.keys" | tee "$handoff_result/session.log"
cmp "$handoff_result/expected.scm" "$handoff_result/work/home/handoff.scm"
cmp "$handoff_result/expected.txt" "$handoff_result/work/home/handoff.txt"
python3 - "$handoff_result/session.log" <<'PY_CHECK'
from pathlib import Path
import re,sys
rows=re.findall(r'STAGE PASS: (\d+) (scheme|write); heap=(\d+); lines=(\d+);',Path(sys.argv[1]).read_text())
assert len(rows)==6 and [r[1] for r in rows]==['scheme','write']*3
assert all(int(lines)>=100 for _,app,_,lines in rows if app=='scheme')
assert len({heap for _,_,heap,_ in rows})==1
PY_CHECK
# Malformed source must fail at a later stage as well as at initial startup.
if "$handoff_binary" --headless --workspace "$handoff_result/work" --screenshots "$handoff_result/shots" \
    --stage scheme /home/handoff.scm "$handoff_result/0.keys" \
    --stage scheme /home/invalid.scm "$handoff_result/2.keys" > "$handoff_result/failure.log" 2>&1; then
    echo 'Failed stage incorrectly returned success' >&2; exit 1
fi
grep -q 'STAGE PASS: 1' "$handoff_result/failure.log"
grep -q 'stage failed' "$handoff_result/failure.log"
echo "Handoff PASS: six real app launches, Japanese saves, fixed heap and cleanup: $handoff_result"
