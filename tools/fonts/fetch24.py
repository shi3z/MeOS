#!/usr/bin/env python3
"""Fetch checksum-pinned inputs for the font comparison, not for normal builds."""
from pathlib import Path
from urllib.request import urlopen
import hashlib
ROOT=Path(__file__).resolve().parents[2]
CACHE=ROOT/'tools/fonts/cache'
INPUTS=[
 ('xfonts-efont-unicode_0.4.2.orig.tar.bz2',
  'https://deb.debian.org/debian/pool/main/x/xfonts-efont-unicode/xfonts-efont-unicode_0.4.2.orig.tar.bz2',
  '7cd38f40f70fb6c6ce7cab46ef00231c2352b1555e31f5c54f5f5683f38baaa2'),
 ('BIZUDGothic-Regular.ttf',
  'https://raw.githubusercontent.com/googlefonts/morisawa-biz-ud-gothic/18934af56b9c003ca58c54bffbf226848cb11032/fonts/ttf/BIZUDGothic-Regular.ttf',
  '7d2b48d84ef4e65c9f85bcf65fb1fb41c92b134be641fab6a7141d597c9a92b0')]
if __name__=='__main__':
 CACHE.mkdir(parents=True,exist_ok=True)
 for name,url,digest in INPUTS:
  target=CACHE/name
  data=target.read_bytes() if target.exists() else urlopen(url,timeout=30).read()
  if hashlib.sha256(data).hexdigest()!=digest:raise SystemExit('checksum mismatch: '+name)
  if not target.exists():target.write_bytes(data)
  print('verified:',name)
