# EPUB math font asset

The EPUB math bitmap strike uses glyphs and OpenType MATH metrics from STIX Two
Math v2.13 b170. The font is licensed under SIL OFL 1.1. Extract
`static_otf/STIXTwoMath-Regular.otf` from the upstream archive and obtain the
pinned W3C operator dictionary XML used by the dictionary generator.
[STIX Two v2.13](https://github.com/stipub/stixfonts/releases/tag/v2.13) ·
[STIX license](https://github.com/stipub/stixfonts/blob/v2.13/OFL.txt) ·
[W3C XML](https://www.w3.org/2003/entities/2007xml/unicode.xml)

```sh
python3 scripts/generate_mathml_font.py \
  --font STIXTwoMath-Regular.otf \
  --dictionary unicode.xml \
  --format cpp \
  --output lib/Epub/Epub/math/MathFontData.inc
```

The generator needs fontTools, a host C compiler, and FreeType development
headers available through `pkg-config`. It verifies both source hashes, then
checks the round trip of every compressed block. FreeType and zlib versions
can affect rasterization and compression output. The 14 ppem strike measured
51,336 bytes with FreeType 2.15.0 and zlib 1.2.12.

The glyph map covers the STIX symbols present in the operator dictionary and
the mathematical Unicode blocks listed by the generator. It also retains all
STIX glyphs needed by MATH variants and assemblies. Ordinary prose uses the
reader font.

MATH adjustments use the font's design-unit values. The four optional
pixel-size corrections in the upstream font are outside the packed format.

## Binary layout

All integers use little-endian byte order and records contain no padding. The
header contains `CIMF`, version `u8`, strike ppem `u8`, units per em `u16`,
and glyph count `u16`. Ten `{u32 offset, u32 size}` pairs follow. Offsets are
relative to the start of the payload after the header.

| Section | Record |
| --- | --- |
| Character map | `{u24 firstScalar, u16 firstGlyph, u8 count}`. Ranges advance both scalar and glyph ID by one. |
| Glyph metrics | Blocked `{u16 advance, i8 left, i8 top, u8 width, u8 height}`. Advance is in font units and the other fields are in strike pixels. |
| Glyph bitmaps | Blocked concatenated 1-bit glyph bitmaps. Each row is MSB-first, padded to a byte boundary. Metrics give each glyph's length. |
| Constants | `i16 minConnectorOverlap` followed by the 56 values of OpenType `MathConstants` in specification order, each stored as `i16`. |
| Italic corrections | Blocked `{u16 glyph, i16 correction}` in font units. |
| Top accent attachments | Blocked `{u16 glyph, i16 attachment}` in font units. |
| Math kern | Blocked `{u16 glyph, u8 cornerMask}`, then each present corner as `u8 heightCount`, that many `i16` heights, and `heightCount + 1` `i16` kern values. Mask bits from low to high are top right, top left, bottom right, and bottom left. |
| Extended shapes | One bit per glyph ID, low bits first. |
| Vertical variants | Blocked construction records described below. |
| Horizontal variants | Blocked construction records with the same format. |

Each construction starts with `{u16 glyph, u8 variantCount, u8 partCount}`.
Variant records are `{u16 glyph, u16 advance}`. For a nonzero part count,
an `i16` assembly italic correction precedes the part records
`{u16 glyph, u16 startConnector, u16 endConnector, u16 fullAdvance, u8 flags}`.
OpenType defines the part flags and these metrics use font units.

A blocked section starts with its `u16` block count and then one descriptor per
block. A descriptor is `{u16 firstGlyph, u16 recordCount, u16 rawSize,
u16 storedSize, u32 offset, u8 encoding}`. Offsets are relative to the data
after the descriptor array. Blocks contain at most 64 records and 2,048
uncompressed bytes. All records are ordered by glyph ID. In sparse sections,
the descriptor's glyph ID is the first key and the records carry their own IDs.

Encoding 0 is raw DEFLATE and encoding 2 is uncompressed. Encoding 1 is raw
DEFLATE of bitmap rows with each row XORed against the previous row. Reconstruct
the rows from top to bottom and reset the previous row to zero for each glyph.
