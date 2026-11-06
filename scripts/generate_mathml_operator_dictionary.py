#!/usr/bin/env python3
"""Generate the firmware's MathML 3 operator table from W3C unicode.xml."""

import argparse
from hashlib import sha256
from pathlib import Path
from xml.etree import ElementTree


# The operator rows in this revision match MathML 3 Appendix C.
SOURCE = "https://www.w3.org/2003/entities/2007xml/unicode.xml"
SOURCE_SHA256 = "bcd6782c078f4f3521300c7d1f7a26ebd766011a62adf45c1111634c2291ac10"
FORMS = {"infix": 0, "prefix": 1, "postfix": 2}
PROPERTIES = {
    "fence": 1,
    "separator": 2,
    "stretchy": 4,
    "symmetric": 8,
    "largeop": 16,
    "movablelimits": 32,
    "accent": 64,
    "linebreakstyle=after": 128,
}
W3C_NOTICE = """W3C SOFTWARE NOTICE AND LICENSE (2002-12-31)
https://www.w3.org/copyright/software-license-2002/

This work (and included software, documentation such as READMEs, or other
related items) is being provided by the copyright holders under the
following license.

License

By obtaining, using and/or copying this work, you (the licensee) agree that
you have read, understood, and will comply with the following terms and
conditions.

Permission to copy, modify, and distribute this software and its
documentation, with or without modification, for any purpose and without
fee or royalty is hereby granted, provided that you include the following
on ALL copies of the software and documentation or portions thereof,
including modifications:

* The full text of this NOTICE in a location viewable to users of the
  redistributed or derivative work.
* Any pre-existing intellectual property disclaimers, notices, or terms and
  conditions. If none exist, the W3C Software Short Notice should be
  included (hypertext is preferred, text is permitted) within the body of
  any redistributed or derivative code.
* Notice of any changes or modifications to the files, including the date
  changes were made. (We recommend you provide URIs to the location from
  which the code is derived.)

Disclaimers

THIS SOFTWARE AND DOCUMENTATION IS PROVIDED "AS IS," AND COPYRIGHT HOLDERS
MAKE NO REPRESENTATIONS OR WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO, WARRANTIES OF MERCHANTABILITY OR FITNESS FOR ANY PARTICULAR
PURPOSE OR THAT THE USE OF THE SOFTWARE OR DOCUMENTATION WILL NOT INFRINGE
ANY THIRD PARTY PATENTS, COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS.

COPYRIGHT HOLDERS WILL NOT BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL OR
CONSEQUENTIAL DAMAGES ARISING OUT OF ANY USE OF THE SOFTWARE OR
DOCUMENTATION.

The name and trademarks of copyright holders may NOT be used in advertising
or publicity pertaining to the software without specific, written prior
permission. Title to copyright in this software and any associated
documentation will at all times remain with copyright holders."""


def read_dictionary(source: Path) -> dict[tuple[str, int], tuple[int, int, int]]:
    xml = source.read_bytes()
    if sha256(xml).hexdigest() != SOURCE_SHA256:
        raise ValueError(f"Expected the pinned unicode.xml at {SOURCE}")
    entries = {}
    for character in ElementTree.fromstring(xml).iter("character"):
        for operator in character.findall("operator-dictionary"):
            text = "".join(chr(int(value)) for value in character.attrib["dec"].split("-"))
            attrs = dict(operator.attrib)
            key = (text, FORMS[attrs.pop("form")])
            left = int(attrs.pop("lspace"))
            right = int(attrs.pop("rspace"))
            attrs.pop("priority")  # Used for authoring/grouping, not rendering.
            flags = 0
            for name, value in attrs.items():
                flags |= PROPERTIES[name if value == "true" else f"{name}={value}"]
            if key in entries:
                raise ValueError(f"Duplicate operator: {key}")
            entries[key] = (left, right, flags)
    if len(entries) != 1105:
        raise ValueError(f"Expected 1105 MathML 3 operator entries, found {len(entries)}")
    return entries


def columns(values: list[str], per_line: int) -> str:
    lines = []
    for index in range(0, len(values), per_line):
        lines.append("    " + ", ".join(values[index : index + per_line]) + ",")
    return "\n".join(lines)


def write_header(output: Path, entries: dict[tuple[str, int], tuple[int, int, int]]) -> None:
    palette = sorted(set(entries.values()))
    if len(palette) > 256:
        raise ValueError("Operator property indices overflow one byte")
    indices = {properties: index for index, properties in enumerate(palette)}

    single_keys = []
    single_properties = []
    text_pool = bytearray()
    offsets = {}
    multi_records = []
    for (text, form), properties in sorted(entries.items()):
        index = indices[properties]
        if len(text) == 1:
            codepoint = ord(text)
            if codepoint >= 0x4000:
                raise ValueError(f"Operator U+{codepoint:04X} overflows its 14-bit field")
            single_keys.append(f"0x{codepoint << 2 | form:04x}")
            single_properties.append(str(index))
            continue

        encoded = text.encode("utf-8")
        if len(encoded) > 7:
            raise ValueError(f"Operator is longer than seven bytes: {text!r}")
        if encoded not in offsets:
            offsets[encoded] = len(text_pool)
            text_pool.extend(encoded)
        multi_records.append(f"{{{offsets[encoded]}, {len(encoded)}, {form}, {index}}}")
    if len(text_pool) > 256:
        raise ValueError("Operator text offsets overflow one byte")

    text_lines = []
    for index in range(0, len(text_pool), 24):
        escaped = "".join(f"\\x{byte:02x}" for byte in text_pool[index : index + 24])
        text_lines.append(f'    "{escaped}"')
    text_literal = "\n".join(text_lines)
    property_rows = [f"{{{left}, {right}, 0x{flags:02x}}}" for left, right, flags in palette]

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        f"""#pragma once

#include <cstdint>

// Generated by scripts/generate_mathml_operator_dictionary.py. Do not edit.
// Copyright David Carlisle 1999-2015
// Source: {SOURCE}
// 2026-11-06: Extracted and packed the operator defaults for firmware use.
// SPDX-License-Identifier: W3C
/*
{W3C_NOTICE}
*/

namespace mathml::operatorDictionary {{

// Spacing is in eighteenth-em units. Flags, low to high: fence, separator,
// stretchy, symmetric, largeop, movablelimits, accent, linebreakstyle=after.
struct Properties {{
  uint8_t lspace;
  uint8_t rspace;
  uint8_t flags;
}};

struct MultiRecord {{
  uint8_t textOffset;
  uint8_t textLength : 3;
  uint8_t form : 2;
  uint8_t propertyIndex;
}};
static_assert(sizeof(MultiRecord) == 3, "Keep the operator dictionary packed");

// clang-format off
inline constexpr Properties PROPERTIES[] = {{
{columns(property_rows, 5)}
}};

// Sorted by (codepoint << 2) | form. Forms: infix (0), prefix (1), postfix (2).
inline constexpr uint16_t SINGLE_KEYS[] = {{
{columns(single_keys, 12)}
}};
inline constexpr uint8_t SINGLE_PROPERTIES[] = {{
{columns(single_properties, 24)}
}};
static_assert(sizeof(SINGLE_KEYS) / sizeof(SINGLE_KEYS[0]) == sizeof(SINGLE_PROPERTIES));

inline constexpr char MULTI_TEXT[] =
{text_literal};

// Sorted by UTF-8 token, then form.
inline constexpr MultiRecord MULTI_RECORDS[] = {{
{columns(multi_records, 5)}
}};
// clang-format on

}}  // namespace mathml::operatorDictionary
""",
        encoding="utf-8",
        newline="\n",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help=f"XML file downloaded from {SOURCE}")
    parser.add_argument("--output", required=True, type=Path, help="Destination C++ header")
    args = parser.parse_args()
    try:
        write_header(args.output, read_dictionary(args.input))
    except (KeyError, OSError, ValueError, ElementTree.ParseError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
