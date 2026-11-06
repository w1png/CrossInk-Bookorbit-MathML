#include "MathFont.h"

namespace mathml {
namespace {

#include "MathFontData.inc"

}  // namespace

std::span<const uint8_t> bundledMathFontBytes() { return STIX_MATH_FONT; }

}  // namespace mathml
