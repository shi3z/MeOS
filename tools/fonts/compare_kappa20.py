#!/usr/bin/env python3
"""Render original Kappa20 BDF glyphs at one framebuffer pixel per font dot."""
from pathlib import Path
import hashlib, json, tarfile
from PIL import Image, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / 'tools/fonts/cache/Kappa20-0.396.tar.bz2'
SHA = '16fc483669da34d94fa7c6cff35b6843e284fa7d96d9b9286349af1124f1f970'

def load(t, name, jis):
    s = t.extractfile('Kappa20-0.396/' + name).read().decode('ascii')
    result = {}
    for block in s.split('STARTCHAR ')[1:]:
        lines = block.splitlines()
        enc = int(next(x.split()[1] for x in lines if x.startswith('ENCODING ')))
        w, h, dx, dy = map(int, next(x[4:] for x in lines if x.startswith('BBX ')).split())
        advance = int(next(x.split()[1] for x in lines if x.startswith('DWIDTH ')))
        try:
            if jis:
                ch = bytes([(enc >> 8) + 128, (enc & 255) + 128]).decode('euc_jp')
            elif enc >= 161:
                ch = bytes([enc]).decode('shift_jis')
            else:
                ch = chr(enc)  # ASCII-friendly mapping of JIS Roman backslash/tilde
        except UnicodeError:
            continue
        start = lines.index('BITMAP') + 1
        rows = [bytes.fromhex(x) for x in lines[start:start+h]]
        result[ord(ch)] = (advance, w, h, dx, dy, rows)
    return result

def draw(im, xy, text, glyphs):
    x, y = xy
    for ch in text:
        advance, w, h, dx, dy, rows = glyphs[ord(ch)]
        top = y + 17 - h - dy
        for row, data in enumerate(rows):
            bits = int.from_bytes(data, 'big')
            for col in range(w):
                if bits & (1 << (len(data)*8-1-col)):
                    im.putpixel((x+dx+col, top+row), (36,39,42))
        x += advance

if __name__ == '__main__':
    assert hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() == SHA
    sample = ['日本語と English を、無理なく読める大きさで。',
              '今日は端末を持って公園へ。考えたことを少しずつ書き留める。',
              '文章を書く道具として、余白と行間も大切にしたい。',
              'ABC xyz 0123456789 / Il1 O0 [] () 「句読点」、。',
              '鬱・薔薇・麒麟・曖昧・髙・﨑・𠮷・😀']
    im = Image.new('RGB', (1280,720), (248,247,242))
    d = ImageDraw.Draw(im); label = ImageFont.load_default(size=18)
    stats = {}
    with tarfile.open(ARCHIVE) as t:
        notice = t.extractfile('Kappa20-0.396/README').read().decode('euc_jp')
        (ROOT/'tools/fonts/licenses/Kappa20-README.txt').write_text(notice)
        for i, (suffix, title) in enumerate([('m','Medium'),('b','Bold')]):
            ja = load(t, 'k20'+suffix+'.bdf', True)
            latin = load(t, '10x20rk'+suffix+'.bdf', False)
            glyphs = ja | latin
            missing = sorted(set(''.join(sample))-set(map(chr,glyphs)))
            stats[title] = dict(jis0208=len(ja), jis0201=len(latin), merged=len(glyphs),
                                missing_in_sample=missing)
            y = 35 + i*330
            d.text((32,y), 'Kappa20 '+title+' / Japanese 20x20 + Latin 10x20 / native pixels / line pitch 32',font=label,fill=(40,60,70))
            for j, line in enumerate(sample):
                # Missing characters are visibly boxed; never silently use another font.
                draw(im,(40,y+45+j*32), ''.join(c if ord(c) in glyphs else '□' for c in line),glyphs)
    out = ROOT/'build-host-sim/font-comparison'
    out.mkdir(parents=True, exist_ok=True)
    im.save(out/'kappa20.png')
    (out/'kappa20-coverage.json').write_text(json.dumps(stats,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(stats,ensure_ascii=False,indent=2))
    print(out/'kappa20.png')
