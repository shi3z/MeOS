#!/usr/bin/env python3
"""Local cmap audit against Python JIS codecs; not glyph-shape conformance."""
from pathlib import Path
import hashlib,json
from fontTools.ttLib import TTFont
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[2]
GJS=Path.home()/'repo/gjs/data'
FONTS={'BIZ UD': [ROOT/'tools/fonts/cache/BIZUDGothic-Regular.ttf'],
       'Noto Sans CJK JP':[GJS/'sources/fonts/NotoSansCJKjp-Regular.otf'],
       'Bunsen Gothic':[GJS/'output/bunsen-build/BunsenGothic-Regular.ttf'],
       'Bunsen + Ext':[GJS/'output/bunsen-build/BunsenGothic-Regular.ttf',GJS/'output/bunsen-build/BunsenGothicExt-Regular.ttf'],
       'Jigmo (3 files)':[GJS/'sources/fonts'/n for n in ['Jigmo.ttf','Jigmo2.ttf','Jigmo3.ttf']]}
def repertoire(kind):
    out=set()
    for a in range(0xa1,0xff):
        for b in range(0xa1,0xff):
            for prefix in ([b'\x8f'] if kind=='0212' else [b'',b'\x8f']):
                try:
                    raw=prefix+bytes([a,b]);codec='euc_jp' if kind=='0212' else 'euc_jis_2004'
                    text=raw.decode(codec)
                    out.add(text)
                except UnicodeDecodeError:pass
    return out
mapping=ROOT/'tools/fonts/cache/jisx0213-2004-std.txt'
rep0213=set()
for line in mapping.read_text().splitlines():
    if not line or line.startswith('#'):continue
    cols=line.split()
    if len(cols)>1 and cols[1].startswith('U+'):
        rep0213.add(''.join(chr(int(cp,16)) for cp in cols[1][2:].split('+')))
sets={'0212':repertoire('0212'), '0213':rep0213}
report={};cmaps={}
for name,paths in FONTS.items():
    cmap={}; ivs=set();files=[]
    for p in paths:
        f=TTFont(p);c=f.getBestCmap();cmap.update({cp:gn for cp,gn in c.items() if gn!='.notdef'})
        for t in f['cmap'].tables:
            if t.format==14:
                for vs,pairs in t.uvsDict.items():ivs.update((cp,vs) for cp,gn in pairs)
        files.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),glyphs=f['maxp'].numGlyphs))
    cmaps[name]=cmap
    item=dict(files=files,unicode_keys=len(cmap),non_pua_keys=sum(not(0xe000<=cp<=0xf8ff or 0xf0000<=cp<=0xffffd or 0x100000<=cp<=0x10fffd) for cp in cmap),ivs_sequences=len(ivs))
    for k,rep in sets.items():
        single={s for s in rep if len(s)==1};multi={s for s in rep if len(s)>1}
        missing=sorted(s for s in single if ord(s) not in cmap)
        item[k]=dict(single_total=len(single),single_present=len(single)-len(missing),missing=missing,multi_sequences=len(multi),multi_components_present=sum(all(ord(c) in cmap for c in s) for s in multi))
    report[name]=item
out=ROOT/'build-host-sim/font-comparison';out.mkdir(parents=True,exist_ok=True)
(out/'outline-coverage.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
for name,r in report.items():print(name,json.dumps({k:v for k,v in r.items() if k!='files' and k not in sets},ensure_ascii=False),[(k,r[k]['single_present'],r[k]['single_total'],r[k]['multi_components_present'],r[k]['multi_sequences']) for k in sets])
sample=['日本語と English を、無理なく読める大きさで。','文章を書く道具。余白と行間、濁点・句読点の見やすさ。','鬱 薔薇 麒麟 曖昧 髙 﨑 𠮷 / ABC xyz 0123456789 Il1 O0']
for size in [24,28,32]:
    im=Image.new('RGB',(1280,720),(248,247,242));d=ImageDraw.Draw(im);label=ImageFont.load_default(size=17)
    for row,name in enumerate(['BIZ UD','Noto Sans CJK JP','Bunsen + Ext']):
        paths=FONTS[name];fonts=[ImageFont.truetype(str(p),size) for p in paths]
        maps=[TTFont(p).getBestCmap() for p in paths]
        y=18+row*235;d.text((32,y),f'{name} / {size}px direct raster / missing = box / no external fallback',font=label,fill=(50,65,75))
        for j,line in enumerate(sample):
            x=40;baseline=y+48+j*(size+12)
            for ch in line:
                idx=next((i for i,m in enumerate(maps) if ord(ch) in m),None)
                if idx is None:
                    d.rectangle((x,baseline-size+3,x+size-3,baseline-1),outline=(140,50,40));x+=size
                else:
                    font=fonts[idx];d.text((x,baseline),ch,font=font,anchor='ls',fill=(36,39,42));x+=font.getlength(ch)
    im.save(out/f'outline-{size}px.png')
