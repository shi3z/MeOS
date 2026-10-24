#!/usr/bin/env python3
"""Native 24-dot bitmap comparison; no rescaling or antialiasing of bitmap rows."""
from pathlib import Path
import argparse, tarfile, json, hashlib
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[2]
ARCHIVE=ROOT/'tools/fonts/cache/xfonts-efont-unicode_0.4.2.orig.tar.bz2'
EXPECTED='7cd38f40f70fb6c6ce7cab46ef00231c2352b1555e31f5c54f5f5683f38baaa2'

def fonts():
    assert hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()==EXPECTED
    result={}
    with tarfile.open(ARCHIVE) as archive:
        for name in ['jiskan24_uni.hex','etl24-unicode.hex']:
            member=next(x for x in archive if x.name.endswith('/'+name))
            lines=archive.extractfile(member).read().decode().splitlines()
            width,height=map(int,lines[0].split()); assert height==24
            rowbytes=(width+7)//8
            for line in lines[1:]:
                if ':' not in line:continue
                cp,bits=line.split(':',1);data=bytes.fromhex(bits)
                assert len(data)==rowbytes*24
                result[int(cp,16)]=(width,data)
    return result

def draw_native(image,xy,text,glyphs,bold=False):
    x,y=xy; fg=(36,39,42)
    for ch in text:
        width,data=glyphs.get(ord(ch),glyphs[0x25a1]);rowbytes=(width+7)//8
        for row in range(24):
            bits=int.from_bytes(data[row*rowbytes:(row+1)*rowbytes],'big')
            for col in range(width):
                if bits & (1<<(rowbytes*8-1-col)):
                    image.putpixel((x+col,y+row),fg)
                    if bold and col+1<width:image.putpixel((x+col+1,y+row),fg)
        x+=width

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    glyphs=fonts();im=Image.new('RGB',(1280,720),(248,247,242));d=ImageDraw.Draw(im);label=ImageFont.load_default(size=18)
    sample=['日本語と English を、無理なく読める大きさで。',
            '今日は端末を持って公園へ。考えたことを少しずつ書き留める。',
            '文章を書く道具として、余白と行間も大切にしたい。',
            'ABC xyz 0123456789 / Il1 O0 [] () 「句読点」、。']
    d.text((32,16),'24-pixel native bitmap vs outline rendering / 1280 x 720 / 1:1 pixels',font=label,fill=(30,40,50))
    titles=['A  Native 24-dot: JIS24 + ETL 12x24 (efont source, unscaled)',
            'B  Same bitmap with 1-pixel horizontal emphasis (preview, not original bold face)',
            'C  BIZ UD Gothic Regular at 24px (outline rasterization, reference only)']
    vector=ImageFont.truetype(str(ROOT/'tools/fonts/cache/BIZUDGothic-Regular.ttf'),24)
    for n,title in enumerate(titles):
        top=58+n*215;d.line((32,top-6,1248,top-6),fill=(207,209,204));d.text((32,top),title,font=label,fill=(60,75,83))
        for line,text in enumerate(sample):
            y=top+32+line*36
            if n<2:draw_native(im,(40,y),text,glyphs,n==1)
            else:d.text((40,y+22),text,font=vector,anchor='ls',fill=(36,39,42))
    args.output.parent.mkdir(parents=True,exist_ok=True);im.save(args.output)
    print('glyphs',len(glyphs),'raw bytes',sum(len(g[1]) for g in glyphs.values()),'image',args.output)
