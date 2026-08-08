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
