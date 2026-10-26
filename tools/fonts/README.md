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
on first use, rather than consuming the internal app partition. The atlas then
stays in an idle PSRAM cache, so switching between Write and Scheme does not
reread roughly 2.4 MiB from microSD. `mem` reports the cache and `mem trim`
releases it. Leaving Home for a non-editor application also trims it
automatically; a failed large editor allocation trims and retries once.
With a Tab5 shell connected, provision it with:

```sh
mkdir /assets/fonts
python3 tools/push_file.py --port /dev/cu.usbmodemXXXX \
  assets/fonts/bizud24.m5f /assets/fonts/bizud24.m5f
```

Both files contain the BIZ UD cmap glyphs rasterized at 24px, with original
advances/baseline offsets. A CRC-covered trailer additionally resolves the
font's OpenType `vrt2` feature during generation and stores 350 addressable
vertical alternates with their `vmtx` advances. The ordinary index and version-1
header remain readable by the previous OTA image, which ignores the trailer.
This file is now the simulator renderer and the Tab5 fallback when the pinned
outline file is unavailable. It remains useful as a fast, deterministic 24px
reference; the normal Tab5 Write path runs FreeType as described below.
Coverage is quantized to 4 bits and stored as either packed nibbles or
independently LZ-compressed glyphs, whichever is smaller.
The source TTF is checksum-pinned by `fetch24.py`; run that script and then
`build_bizud.py` to regenerate (Pillow 11.3.0 / fontTools 4.60.2 used here; rendering
can vary across FreeType versions). Keep `licenses/BIZ-UD-OFL.txt` with distributions.
The generated coverage is about 2.28 MB plus the glyph indexes; no other typeface is
merged or used for fallback. Unknown characters display a box. Full JIS combining
sequence and IVS shaping is not implemented by this first simple renderer.

## Runtime OpenType path

`fontbench [12..48]` compares the fixed 24px atlas with FreeType rendering from
`/assets/fonts/BIZUDGothic-Regular.ttf`, including side-by-side horizontal and
vertical samples. It keeps the fallback atlas unchanged, uses FreeType's
hinted grayscale renderer, quantizes each result to the same 4-bit coverage as
the atlas, reads font tables on demand through a 16-way 16 KiB VFS stream cache, and
retains up to 384 glyphs in a bounded PSRAM cache. Serial output reports face
open, actual SD bytes/reads, cold draw, warm draw, raster and cache figures.
Press Q to return. Provision the currently pinned source without committing the cache:

```sh
python3 tools/push_file.py --port /dev/cu.usbmodemXXXX \
  tools/fonts/cache/BIZUDGothic-Regular.ttf \
  /assets/fonts/BIZUDGothic-Regular.ttf
python3 tools/fonts/outline_device_test.py /dev/cu.usbmodemXXXX \
  --size 24 --leave-open
python3 tools/fonts/outline_device_test.py /dev/cu.usbmodemXXXX \
  --size 24 --trim-sd-cache
python3 tools/fonts/write_outline_device_test.py /dev/cu.usbmodemXXXX \
  --file /home/note.txt --leave-open
```

The first device measurement used an in-memory face to qualify rendering and
cache behavior independently; it took about nine seconds to read the complete
4.45 MiB file. The default VFS-backed stream avoids that unconditional read and
keeps 256 KiB of source windows. Write promotes only diverse long documents
(at least 8 KiB and 256 distinct non-ASCII characters) to a resident face; use
`write_layout_device_test.py` to exercise that path with 900 CJK characters and
report logical littlefs reads, bounded asset-cache hits and physical SD requests.
On the qualified Tab5/card, the optional 1,170 KiB SD asset cache reduced that
cold workload from 9.947 s to 3.487 s; allocation failure retains direct I/O.
The `--trim-sd-cache` device path checks that `mem trim` returns about 1,170 KiB,
the following outline load recreates the cache, and application memory is still
released on exit.
For repeated trim/recreate, font-size and Write cleanup checks, use
`tools/sdroot/cache_lifecycle_device.py PORT --cycles 20 --log NEW_LOG_PATH`.
Its optional `--boot-contention` mode resets the device and requires cache
release during the background IME dictionary read. See `docs/sd-root-device.md`
for prerequisites and the limits of that test.
Write starts at 24px and offers 20/24/28/32px live choices with F3/F4. The 350-entry `vrt2`
glyph-ID map is generated by `build_otfont_vertical.py` from the checksum-pinned
font; FreeType supplies the outlines and size-specific horizontal and vertical
metrics. The path accepts only this operator-provisioned asset and is not yet
an arbitrary-font loader or a shaping engine.

FreeType is resolved as the pinned Espressif component in `dependencies.lock`
and used under the FreeType License. Portions of this software are copyright
2026 The FreeType Project (https://freetype.org). All rights reserved. Its
complete unmodified license is included in the resolved component archive.

The encoder searches up to 32 recent matches for each three-byte prefix.
`python3 tools/fonts/check_bizud.py` independently decodes all 11,741 ordinary
glyphs and 350 vertical alternates and checks separate digests of their metrics
and pixels. The ordinary bank remains byte-for-byte visually compatible with
the version-1 atlas.
