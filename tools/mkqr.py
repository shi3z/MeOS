#!/usr/bin/env python3
"""Makes the QR code that installs a MeOS app:   tools/mkqr.py URL [app.wasm] [-o out.png]

With the .wasm file the code carries its SHA-256, so the Tab5 installs exactly that file (meos://install?url=..&sha256=..).
Without it the code is the plain link.  Needs:  pip install qrcode pillow
"""
import hashlib, sys, urllib.parse
import qrcode

args = [a for a in sys.argv[1:] if a != "-o"]
out = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else "app_qr.png"
if "-o" in sys.argv: args.remove(out)
if not args:
    sys.exit(__doc__)
url = args[0]
if len(args) > 1:
    digest = hashlib.sha256(open(args[1], "rb").read()).hexdigest()
    text = "meos://install?url=%s&sha256=%s" % (urllib.parse.quote(url, safe=":/"), digest)
else:
    text = url
img = qrcode.make(text, error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=12, border=4)
img.save(out)
print(text)
print("wrote", out)
