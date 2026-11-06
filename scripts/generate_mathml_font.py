#!/usr/bin/env python3
"""Pack a licensed STIX Two Math v2.13 strike for the EPUB equation renderer.

Requires fonttools, a host C compiler, and FreeType development headers
discoverable by pkg-config. The font source and dictionary must match the
published inputs. The binary format is described in docs/mathml-font.md.
"""

import argparse
from hashlib import sha256
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.otData import otData

from generate_mathml_operator_dictionary import SOURCE as DICTIONARY_SOURCE, read_dictionary


FONT_SOURCE = "https://raw.githubusercontent.com/stipub/stixfonts/v2.13/zipfiles/static_otf.zip"
FONT_SHA256 = "f2076b9f1676438439dd41e23676f5ab99056e83d6b8f8c27841591ef2ccfa72"
MAX_BLOCK_BYTES = 2048
MAX_GROUP_GLYPHS = 64
KERN_CORNERS = ("TopRightMathKern", "TopLeftMathKern", "BottomRightMathKern", "BottomLeftMathKern")
MATH_BLOCKS = (
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x2070, 0x209F),  # Superscripts and subscripts
    (0x2100, 0x218F),  # Letterlike symbols and number forms
    (0x2190, 0x23FF),  # Arrows, mathematical operators, and miscellaneous technical
    (0x25A0, 0x25FF),  # Geometric shapes
    (0x27C0, 0x27FF),  # Miscellaneous mathematical symbols-A and supplemental arrows-A
    (0x2900, 0x2BFF),  # Supplemental arrows-B, mathematical symbols-B, and miscellaneous symbols and arrows
    (0x1D400, 0x1D7FF),  # Mathematical alphanumerics
    (0x1EE00, 0x1EEFF),  # Arabic mathematical alphanumerics
)


def rasterize(font_path, ppem, glyph_names):
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "freetype2"], text=True)
    helper = Path(__file__).with_name("raster_mathml_font.c")
    with tempfile.TemporaryDirectory(prefix="crossink-math-font-") as directory:
        rasterizer = Path(directory) / "raster"
        subprocess.run(["cc", "-O2", helper, "-o", rasterizer, *shlex.split(flags)], check=True)
        data = subprocess.check_output([rasterizer, font_path, str(ppem)])

    glyphs = {}
    offset = 0
    for name in glyph_names:
        left, top, width, height = struct.unpack_from("<hhhh", data, offset)
        offset += 8
        stride = (width + 7) // 8
        end = offset + stride * height
        if end > len(data):
            raise ValueError(f"Truncated raster for {name}")
        glyphs[name] = (left, top, width, height, data[offset:end])
        offset = end
    if offset != len(data):
        raise ValueError("Glyph order does not match the FreeType raster output")
    return glyphs


def deflate(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def xor_rows(bitmap, stride):
    if not stride:
        return bitmap
    return bytes(value ^ (bitmap[index - stride] if index >= stride else 0)
                 for index, value in enumerate(bitmap))


def pack_blocks(records):
    """Pack sorted records into independently seekable bounded blocks."""
    index = bytearray()
    payload = bytearray()
    group = []
    group_size = 0

    def append_group():
        raw = b"".join(record for _, record, _ in group)
        if not 0 < len(raw) <= MAX_BLOCK_BYTES:
            raise ValueError("Invalid uncompressed math font block size")
        compressed = deflate(raw)
        flags = 0
        if any(stride for _, _, stride in group):
            differenced = b"".join(xor_rows(record, stride) for _, record, stride in group)
            candidate = deflate(differenced)
            if len(candidate) < len(compressed):
                restored = bytearray(zlib.decompress(candidate, -15))
                start = 0
                for _, record, stride in group:
                    for offset in range(start + stride, start + len(record)):
                        restored[offset] ^= restored[offset - stride]
                    start += len(record)
                if restored != raw:
                    raise ValueError("Math font row-delta round trip failed")
                compressed, flags = candidate, 1
        if len(compressed) >= len(raw):
            compressed, flags = raw, 2
        if flags == 0 and zlib.decompress(compressed, -15) != raw:
            raise ValueError("Math font DEFLATE round trip failed")
        index.extend(struct.pack("<HHHHIB", group[0][0], len(group), len(raw), len(compressed), len(payload), flags))
        payload.extend(compressed)

    for glyph_id, record, stride in records:
        if group and (group_size + len(record) > MAX_BLOCK_BYTES or len(group) == MAX_GROUP_GLYPHS):
            append_group()
            group.clear()
            group_size = 0
        group.append((glyph_id, record, stride))
        group_size += len(record)
    if group:
        append_group()
    return struct.pack("<H", len(index) // struct.calcsize("<HHHHIB")) + index + payload


def codepoint_ranges(cmap, glyph_ids):
    output = bytearray()
    start = previous_codepoint = -2
    first_glyph = previous_glyph = -2
    for codepoint, name in cmap.items():
        glyph_id = glyph_ids[name]
        if codepoint != previous_codepoint + 1 or glyph_id != previous_glyph + 1 or codepoint - start >= 255:
            if start >= 0:
                output.extend(start.to_bytes(3, "little"))
                output.extend(struct.pack("<HB", first_glyph, previous_codepoint - start + 1))
            start, first_glyph = codepoint, glyph_id
        previous_codepoint, previous_glyph = codepoint, glyph_id
    if start >= 0:
        output.extend(start.to_bytes(3, "little"))
        output.extend(struct.pack("<HB", first_glyph, previous_codepoint - start + 1))
    return output


def sparse_values(coverage, records, glyph_ids):
    return sorted((glyph_ids[name], struct.pack("<Hh", glyph_ids[name], record.Value), 0)
                  for name, record in zip(coverage.glyphs, records) if name in glyph_ids)


def kern_records(info, glyph_ids):
    output = []
    for name, record in zip(info.MathKernCoverage.glyphs, info.MathKernInfoRecords):
        if name not in glyph_ids:
            continue
        glyph_id = glyph_ids[name]
        corners = [getattr(record, name) for name in KERN_CORNERS]
        flags = sum(1 << index for index, corner in enumerate(corners) if corner is not None)
        data = bytearray(struct.pack("<HB", glyph_id, flags))
        for corner in corners:
            if corner is not None:
                data.append(corner.HeightCount)
                points = [value.Value for value in corner.CorrectionHeight + corner.KernValue]
                data.extend(struct.pack(f"<{len(points)}h", *points))
        output.append((glyph_id, data, 0))
    return sorted(output)


def variant_records(coverage, constructions, glyph_ids):
    output = []
    for name, construction in zip(coverage.glyphs, constructions):
        glyph_id = glyph_ids[name]
        variants = construction.MathGlyphVariantRecord
        assembly = construction.GlyphAssembly
        parts = assembly.PartRecords if assembly else []
        data = bytearray(struct.pack("<HBB", glyph_id, len(variants), len(parts)))
        for variant in variants:
            data.extend(struct.pack("<HH", glyph_ids[variant.VariantGlyph], variant.AdvanceMeasurement))
        if assembly:
            data.extend(struct.pack("<h", assembly.ItalicsCorrection.Value))
        for part in parts:
            data.extend(struct.pack("<HHHHB", glyph_ids[part.glyph], part.StartConnectorLength,
                                    part.EndConnectorLength, part.FullAdvance, part.PartFlags))
        output.append((glyph_id, data, 0))
    return sorted(output)


def glyph_order(cmap, variants):
    names = list(dict.fromkeys(cmap.values()))
    extra = set(variants.VertGlyphCoverage.glyphs + variants.HorizGlyphCoverage.glyphs)
    for construction in variants.VertGlyphConstruction + variants.HorizGlyphConstruction:
        extra.update(record.VariantGlyph for record in construction.MathGlyphVariantRecord)
        if construction.GlyphAssembly:
            extra.update(record.glyph for record in construction.GlyphAssembly.PartRecords)
    names.extend(sorted(extra - set(names)))
    return names


def pack_asset(font, cmap, rasters, ppem):
    math = font["MATH"].table
    variants = math.MathVariants
    names = glyph_order(cmap, variants)
    glyph_ids = {name: glyph_id for glyph_id, name in enumerate(names)}
    metrics = []
    bitmaps = []
    for glyph_id, name in enumerate(names):
        left, top, width, height, pixels = rasters[name]
        metrics.append((glyph_id, struct.pack("<HbbBB", font["hmtx"][name][0], left, top, width, height), 0))
        bitmaps.append((glyph_id, pixels, (width + 7) // 8))

    constants = [variants.MinConnectorOverlap]
    fields = next(fields for name, fields in otData if name == "MathConstants")
    for _, name, *_ in fields:
        value = getattr(math.MathConstants, name)
        constants.append(getattr(value, "Value", value))
    info = math.MathGlyphInfo
    extended_shapes = bytearray((len(names) + 7) // 8)
    for name in info.ExtendedShapeCoverage.glyphs:
        if name in glyph_ids:
            glyph_id = glyph_ids[name]
            extended_shapes[glyph_id // 8] |= 1 << (glyph_id % 8)
    italics = info.MathItalicsCorrectionInfo
    accents = info.MathTopAccentAttachment
    sections = {
        "cmap": codepoint_ranges(cmap, glyph_ids),
        "metric": pack_blocks(metrics),
        "bitmap": pack_blocks(bitmaps),
        "constants": struct.pack(f"<{len(constants)}h", *constants),
        "italics": pack_blocks(sparse_values(italics.Coverage, italics.ItalicsCorrection, glyph_ids)),
        "accents": pack_blocks(sparse_values(accents.TopAccentCoverage, accents.TopAccentAttachment, glyph_ids)),
        "kern": pack_blocks(kern_records(info.MathKernInfo, glyph_ids)),
        "extended": extended_shapes,
        "vvariants": pack_blocks(variant_records(variants.VertGlyphCoverage, variants.VertGlyphConstruction, glyph_ids)),
        "hvariants": pack_blocks(variant_records(variants.HorizGlyphCoverage, variants.HorizGlyphConstruction, glyph_ids)),
    }
    header = bytearray(struct.pack("<4sBBHH", b"CIMF", 1, ppem, font["head"].unitsPerEm, len(names)))
    payload = bytearray()
    for name, content in sections.items():
        header.extend(struct.pack("<II", len(payload), len(content)))
        payload.extend(content)
        print(f"{name:<10} {len(content):>6} bytes")
    return header + payload, len(names)


def write_asset(destination, asset, output_format):
    if output_format == "binary":
        destination.write_bytes(asset)
        return
    lines = [
        "// Generated by scripts/generate_mathml_font.py. See STIX-OFL.txt.",
        f"// SHA256 {sha256(asset).hexdigest()}",
        "alignas(4) constexpr uint8_t STIX_MATH_FONT[] = {",
    ]
    for index in range(0, len(asset), 16):
        values = ", ".join(f"0x{value:02x}" for value in asset[index:index + 16])
        lines.append(f"    {values},")
    lines.append("};")
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=Path, help=f"STIXTwoMath-Regular.otf extracted from {FONT_SOURCE}")
    parser.add_argument("--dictionary", required=True, type=Path, help=f"unicode.xml from {DICTIONARY_SOURCE}")
    parser.add_argument("--output", required=True, type=Path, help="Destination CIMF binary or C++ include")
    parser.add_argument("--format", choices=("binary", "cpp"), default="binary")
    parser.add_argument("--ppem", type=int, choices=(14, 16), default=14)
    args = parser.parse_args()
    try:
        if sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
            raise ValueError("Expected the pinned STIX Two Math v2.13 font")
        operators = read_dictionary(args.dictionary)
        operator_codepoints = {ord(character) for token, _ in operators for character in token}
        with TTFont(args.font) as font:
            cmap = {cp: name for cp, name in sorted(font.getBestCmap().items())
                    if cp in operator_codepoints or any(lo <= cp <= hi for lo, hi in MATH_BLOCKS)}
            rasters = rasterize(args.font, args.ppem, font.getGlyphOrder())
            asset, glyph_count = pack_asset(font, cmap, rasters, args.ppem)
        write_asset(args.output, asset, args.format)
    except (OSError, subprocess.CalledProcessError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")
    print(f"{len(cmap)} codepoints, {glyph_count} glyphs, {len(asset)} bytes, SHA256 {sha256(asset).hexdigest()}")


if __name__ == "__main__":
    main()
