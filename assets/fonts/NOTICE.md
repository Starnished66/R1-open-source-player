# cjk_cyrillic.ttf -- provenance and license

This font is a derivative of **Noto Sans CJK SC** ("Noto Sans S Chinese
DemiLight" / `NotoSansHans-DemiLight`), copyright (c) 2014 Adobe Systems
Incorporated, with contributions credited to Ryoko Nishizuka (kana &
ideographs), Paul D. Hunt (Latin, Greek & Cyrillic), Wenlong Zhang
(bopomofo), and Sandoll Communication (hangul). "Noto" is a trademark of
Google Inc.

Licensed under the **Apache License, Version 2.0**:
http://www.apache.org/licenses/LICENSE-2.0.html

A verbatim copy of that same font ships as `/usr/resource/fonts/default.otf`
in the HiBy R1's stock firmware (which is how it was located and confirmed
Apache-2.0-licensed here -- its embedded `name` table entries state the
license directly).

## Modifications made for this project

This file is **not** a verbatim copy of that font. Two changes were made,
both offline (no build step in this repo regenerates it):

1. **Subsetted** to Cyrillic (U+0400-04FF), General Punctuation (U+2000-206F,
   for curly quotes/dashes/ellipsis), CJK Symbols and Punctuation
   (U+3000-303F), Japanese kana (U+3040-30FF), Halfwidth/Fullwidth Forms
   (U+FF00-FFEF), and Japanese kanji -- via `fonttools subset`. Everything
   else (the original's Latin, Greek, Vietnamese-extended, and other
   CJK-adjacent coverage) was dropped; this project's own Latin text uses
   its own bitmap fonts and never needs this file's fallback for it.

   Kanji coverage was later narrowed further, from the full ~27,500-character
   Unicode CJK Unified Ideographs + Extension A repertoire down to just the
   2,136-character **Joyo kanji** list (Japan's official "common use"
   standard, 2010 revision) -- real-device repack testing found the
   full-coverage version added ~9MB to the flash image (glyph outline data
   compresses poorly), pushing the repacked firmware image over its 45MB
   size limit. The Joyo codepoint list itself came from x0213.org's own
   published character-code table (https://x0213.org/joyo-kanji-code/,
   "distribution unlimited"), not hand-picked. This is a real, known
   tradeoff -- any song/artist/album tag using a kanji outside Joyo
   (uncommon proper nouns, place names, older readings) shows a blank glyph
   instead of falling through to a different working font. See
   `src/fallback_font.c`'s own comment for the full reasoning and the next
   step up in coverage (JIS X 0208, ~6,355 kanji) if the flash budget ever
   allows revisiting this.
2. **Outlines converted from CFF (PostScript) to TrueType** (`glyf`) --
   via `fonttools`/`otf2ttf`'s cubic-to-quadratic curve conversion. LVGL's
   lighter `tiny_ttf` renderer (used for this file, see `src/fallback_font.c`)
   only reads TrueType outlines, not CFF; the original ships as CFF.

Per Apache License 2.0 section 4(b), this NOTICE documents that the file
has been modified from the original.

# thai.ttf -- provenance and license

A subset of **CS ChatThaiUI** by Chanok Samiti (BoonUni), copyright (c) 2014.
A verbatim copy ships as `/usr/resource/fonts/Thai.ttf` in the HiBy R1's
stock firmware. Its embedded `name` table states the license directly:

Licensed under **Creative Commons Attribution 4.0 International**
(CC BY 4.0): http://creativecommons.org/licenses/by/4.0/

## Modifications made for this project

**Subsetted** to the Thai Unicode block (U+0E00-0E7F) via `fonttools
subset`; already TrueType outlines, so no outline conversion was needed.
Used only by the host build (`make host`) so the desktop simulator can
exercise the same fallback chain as the real device -- on target, this
project reads `/usr/resource/fonts/Thai.ttf` directly from the stock
firmware at runtime rather than bundling a copy (see `src/fallback_font.c`).

# emoji.ttf -- provenance and license

A subset of **Noto Emoji** (the monochrome/outline emoji family -- distinct
from "Noto Color Emoji"), copyright 2013 Google LLC, from the
`google/fonts` repository (`ofl/notoemoji/NotoEmoji[wght].ttf`). "Noto" is
a trademark of Google Inc.

Licensed under the **SIL Open Font License, Version 1.1** (OFL):
https://scripts.sil.org/OFL

Not stock-firmware content -- HiBy's own firmware has no emoji font to
piggyback on (unlike Thai.ttf/Korean.ttf below, or Latin/CJK/Cyrillic
coverage). This project ships its own copy for both host and target.
`assets/fonts/emoji.ttf` (this file) is a symlink to the single real copy
at `firmware/overlay/usr/resource/fonts/emoji.ttf`, which
`scripts/repack_upt.sh` copies onto the device's `/usr/resource/fonts/`
directory as part of a full firmware repack -- the same mechanism this
project already uses to ship other files it owns that aren't in stock
(e.g. `usr/bin/sync_ntp.sh`). A standalone player-binary update (SD-card
`.open_hiby_player/open_hiby_player` swap, or `adb push`) does not carry
this file; on a device that never received a full repack (or the R3 Pro
II, which has no staging-image/repack step at all), the emoji fallback
face is simply absent and every other font/rendering behavior is
unaffected -- see `src/ui/fallback_font.c`'s own comment on why loading
this face is optional, not required, unlike CJK/Korean/Thai.

## Modifications made for this project

The upstream file is a **variable font** (`wght` axis, 300-700). Two
changes were made, both offline (no build step in this repo regenerates
them):

1. **Instantiated to a static Regular (`wght=400`) instance** via
   `fonttools varLib.instancer` -- LVGL's `tiny_ttf` renderer (used for
   this file, see `src/ui/fallback_font.c`) does not apply `gvar` variation
   deltas, so a variable font's un-instantiated glyphs would render at
   whatever the font's own default outline happens to be, not necessarily
   Regular weight.
2. **Subsetted to the full Unicode `emoji-test.txt` v16.0 registry**
   (https://unicode.org/Public/emoji/16.0/emoji-test.txt) -- every code
   point appearing in any fully-qualified or component emoji sequence,
   1,445 unique code points total, plus U+FE0E (text presentation
   selector), U+FE0F (emoji presentation selector), and U+200D
   (zero-width joiner) explicitly. The source font already ships these
   three as real, zero-contour/zero-advance-width glyphs (confirmed by
   direct inspection, not assumed) -- keeping them mapped in the subset
   means a variation-selector-qualified emoji (e.g. U+2764 U+FE0F, a red
   heart) renders as just the base glyph with no stray placeholder box
   next to it, since LVGL finds a real (if invisible) glyph for the
   selector instead of falling through the whole fallback chain to a
   missing-glyph placeholder. `GSUB`/`STAT` tables dropped (no ligature
   substitution support in this renderer to make use of them). Confirmed
   via `fontTools` table inspection that both the pre-subset instance and
   the final subset file contain `glyf` outline data with no `CFF `/
   `COLR`/`CBDT` tables -- a genuine monochrome TrueType outline font this
   renderer can actually rasterize, not a color/CFF font that would
   silently fail to load.

Per OFL 1.1 section 5, this file is a modified version and must not be
sold by itself, nor distributed under the unmodified font's original
name -- it isn't (it ships only as part of this project, named
`emoji.ttf`).

Rendering ceiling, same as CJK/Thai: monochrome outline glyphs only, one
glyph per Unicode code point. No color emoji (this renderer has no
COLR/CBDT support) and no compound/ZWJ-sequence fused glyphs (no OpenType
GSUB/shaping support) -- a family emoji or flag sequence renders as
several adjacent simple glyphs, not one composed image. Accepted,
disclosed limitation, not a defect.

# Korean text support -- not bundled here

The HiBy R1's stock firmware also ships a Korean font
(`/usr/resource/fonts/Korean.ttf`, NanumGothic Bold, copyright NHN
Corporation 2011). On target, this project reads that file directly at
runtime the same way it does for Thai -- nothing Korean-specific is
redistributed by this app. A subsetted copy is deliberately **not**
included here for the host build: unlike Thai.ttf, this particular file's
own embedded metadata doesn't state a license (Naver's Nanum font family is
widely distributed under SIL OFL 1.1 elsewhere, but that isn't confirmed
from this file itself), so it's excluded from this public repo pending
verification. The desktop simulator therefore won't render Korean text
locally; this has no effect on the real device, where Korean.ttf is never
copied or modified, only read from its existing on-device location.
