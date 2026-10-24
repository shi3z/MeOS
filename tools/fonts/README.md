# Font research and BIZ UD writing font

The writing UI needs readable, harmonious Japanese/Latin at approximately 24 device
pixels. The first writing implementation now uses BIZ UD Gothic Regular 1.051,
rasterized directly at 24px with 16 coverage levels; native bitmap comparisons
below remain research references.

`compare24.py` uses native 24-row data from the upstream efont 0.4.2 source archive:

- `jiskan24_uni.hex`: Japanese/JIS bitmap patterns, 24x24.
- `etl24-unicode.hex`: Latin and other narrow glyphs, 12x24.
- Narrow glyphs override overlapping wide forms, as appropriate for mixed-width text.

The combination contains 8,468 codepoints and 567,480 bitmap bytes before an index
or compression. Sample A plots each source bit as exactly one output pixel. Sample B
adds a one-pixel horizontal emphasis for comparison; it is not a supplied bold face.
Sample C is BIZ UD Gothic Regular rasterized at 24px by Pillow/FreeType, solely as an
outline-font reference. It is not a simulation of an embedded rasterizer. All three
use the same mixed Japanese/Latin text and 36px line spacing on a 1280x720 canvas.

Reproduce (requires Pillow, tested with 11.3.0):

```sh
python3 tools/fonts/fetch24.py
python3 tools/fonts/compare24.py --output build-host-sim/font-comparison/24px.png
```

Inputs are checksum-pinned. Downloads stay in ignored `cache/`; they are not normal
simulator build inputs. `sources24.json` records the bitmap archive and selected
members. Unmodified upstream README/COPYRIGHT and BIZ UD OFL text are in `licenses/`.
The efont README describes JIS24 and ETL24 as Public Domain, and its COPYRIGHT
provides the efont project's BSD-style terms. These statements should be retained
with any selected derivative. No blanket license is inferred for other members
of the composite efont collection or for the MeOS codebase.

Visual observations: the native JIS24 sample is crisp and visibly Mincho/serif-like;
its Latin is more terminal-like. The synthetic emphasis darkens verticals and can
close counters. The outline reference is also quite legible on the host. Native
bitmap design alone does not establish superior readability or Latin/Japanese
harmony. Do not silently adopt this pair or synthesize a bold default merely from
its availability. Japanese-specific glyph shapes, ordinary-prose coverage, stroke
weight and the physical 5-inch Tab5 should guide selection.

## Kappa20 native comparison

`compare_kappa20.py` renders original medium and bold BDF glyphs without scaling,
anti-aliasing, synthetic emboldening, or font fallback. Japanese cells are 20x20;
Latin/JIS X 0201 cells are 10x20; baseline ascent is 17 and line pitch is 32.

Source archive: http://distcache.FreeBSD.org/local-distfiles/hrs/Kappa20-0.396.tar.bz2
(save as `tools/fonts/cache/Kappa20-0.396.tar.bz2`). The script verifies SHA-256
`16fc483669da34d94fa7c6cff35b6843e284fa7d96d9b9286349af1124f1f970`, matching
FreeBSD ports `japanese/font-kappa20/distinfo`, before reading any font data.
Run `python3 tools/fonts/compare_kappa20.py` (requires Pillow). Output goes to
`build-host-sim/font-comparison/kappa20.png` and `kappa20-coverage.json`.
The original Japanese README is retained in `licenses/Kappa20-README.txt`.

Actual BDF counts in both weights: JIS X 0208 6,879 glyphs, of which 6,355 are
kanji and 524 are non-kanji; JIS X 0201 159 glyphs. This preview maps the latter
to ASCII plus halfwidth kana, yielding 7,038 Unicode keys. This uses ASCII
backslash/tilde semantics, rather than strict JIS Roman yen/overline semantics.
The upstream README says 6,912 JIS X 0208 characters; that differs from the
actual BDF CHARS and ENCODING counts, so use the measured 6,879 for embedding.
The archive-wide design count in the README includes other encodings and forms;
it must not be interpreted as unique Japanese characters.

Missing sample characters are drawn as an explicit square: 髙, 﨑, 𠮷, 😀.
This is a research preview, not a firmware font integration or a device check.

## Outline coverage and native-size samples

`python3 tools/fonts/audit_outline.py` uses fontTools and Pillow, plus the local
`~/repo/gjs/data` artifacts (read only). BIZ UD uses the checksum-pinned 1.051
file above. Noto Sans CJK JP local file reports version 2.004. Each inspected
file path, hash and glyph count is recorded in `outline-coverage.json` alongside
24/28/32px images in `build-host-sim/font-comparison/`. Samples have no external
fallback; missing characters are explicit red boxes. All text is rendered at the
requested pixel size, not scaled from a smaller raster.

Fetch `https://x0213.org/codetable/jisx0213-2004-std.txt` into
`tools/fonts/cache/jisx0213-2004-std.txt` before running. This Project X0213 mapping
is freely usable/modifiable/distributable (notice retained in cached input).
Coverage uses its default Unicode column (11,208 single-codepoint entries and
25 multi-codepoint sequences). JIS X 0212 uses Python's `euc_jp` decoder over
its 94x94 supplementary plane (6,067 distinct Unicode characters). The audit
checks cmap entries, not visual correctness, JIS regional forms, successful
combining shaping or IVS rendering. Component availability for 25 combining
sequences is reported separately and must not be described as shaping success.
Bunsen coverage includes PUA aliases; `non_pua_keys` is reported separately.
Bunsen Ext and Jigmo's three files are unions, not a single-font glyph count.

## Selected writing font

`drivers/lcd/bizud24_data.inc` is the generated, committed simulator input (no Python,
FreeType or TTF runtime). `assets/fonts/bizud24.m5f` contains the identical atlas
for Tab5; copy it to that absolute path on the persistent SD root before opening
the writing editor. The firmware loads and CRC-checks the atlas into PSRAM only
while the editor is active, rather than consuming the internal app partition.
With a Tab5 shell connected, provision it with:

```sh
mkdir /assets/fonts
python3 tools/push_file.py --port /dev/cu.usbmodemXXXX \
  assets/fonts/bizud24.m5f /assets/fonts/bizud24.m5f
```

Both files contain the BIZ UD cmap glyphs rasterized
at 24px, with original advances/baseline offsets. Coverage is quantized to 4 bits
and stored as either packed nibbles or independently LZ-compressed glyphs, whichever is smaller.
The source TTF is checksum-pinned by `fetch24.py`; run that script and then
`build_bizud.py` to regenerate (Pillow 11.3.0 / fontTools 4.60.2 used here; rendering
can vary across FreeType versions). Keep `licenses/BIZ-UD-OFL.txt` with distributions.
The generated coverage is about 2.38 MB plus the glyph index; no other typeface is
merged or used for fallback. Unknown characters display a box. Full JIS combining
sequence and IVS shaping is not implemented by this first simple renderer.

The encoder searches up to 32 recent matches for each three-byte prefix, keeping
exactly the existing runtime format. This saves 135,138 bytes versus the initial
atlas. `python3 tools/fonts/check_bizud.py` independently decodes all 11,741 glyphs
and checks a digest of their metrics and pixels against the original atlas. No
characters, coverage levels, sizes or fallback behavior changed.
