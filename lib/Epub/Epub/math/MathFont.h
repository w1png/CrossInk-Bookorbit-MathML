#pragma once

#include <InflateReader.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace mathml {

std::span<const uint8_t> bundledMathFontBytes();

// One scratch instance per active layout or render task. Allocate it on demand
// for books with formulas, outside the constrained parser/render task stacks.
class MathFont {
 public:
  using Glyph = uint16_t;
  static constexpr Glyph MissingGlyph = UINT16_MAX;

  enum class Constant : uint8_t {
    MinConnectorOverlap,
    ScriptPercentScaleDown,
    ScriptScriptPercentScaleDown,
    DelimitedSubFormulaMinHeight,
    DisplayOperatorMinHeight,
    MathLeading,
    AxisHeight,
    AccentBaseHeight,
    FlattenedAccentBaseHeight,
    SubscriptShiftDown,
    SubscriptTopMax,
    SubscriptBaselineDropMin,
    SuperscriptShiftUp,
    SuperscriptShiftUpCramped,
    SuperscriptBottomMin,
    SuperscriptBaselineDropMax,
    SubSuperscriptGapMin,
    SuperscriptBottomMaxWithSubscript,
    SpaceAfterScript,
    UpperLimitGapMin,
    UpperLimitBaselineRiseMin,
    LowerLimitGapMin,
    LowerLimitBaselineDropMin,
    StackTopShiftUp,
    StackTopDisplayStyleShiftUp,
    StackBottomShiftDown,
    StackBottomDisplayStyleShiftDown,
    StackGapMin,
    StackDisplayStyleGapMin,
    StretchStackTopShiftUp,
    StretchStackBottomShiftDown,
    StretchStackGapAboveMin,
    StretchStackGapBelowMin,
    FractionNumeratorShiftUp,
    FractionNumeratorDisplayStyleShiftUp,
    FractionDenominatorShiftDown,
    FractionDenominatorDisplayStyleShiftDown,
    FractionNumeratorGapMin,
    FractionNumDisplayStyleGapMin,
    FractionRuleThickness,
    FractionDenominatorGapMin,
    FractionDenomDisplayStyleGapMin,
    SkewedFractionHorizontalGap,
    SkewedFractionVerticalGap,
    OverbarVerticalGap,
    OverbarRuleThickness,
    OverbarExtraAscender,
    UnderbarVerticalGap,
    UnderbarRuleThickness,
    UnderbarExtraDescender,
    RadicalVerticalGap,
    RadicalDisplayStyleVerticalGap,
    RadicalRuleThickness,
    RadicalExtraAscender,
    RadicalKernBeforeDegree,
    RadicalKernAfterDegree,
    RadicalDegreeBottomRaisePercent,
    Count,
  };

  enum class Corner : uint8_t { TopRight, TopLeft, BottomRight, BottomLeft };
  enum class Direction : uint8_t { Vertical, Horizontal };

  struct Metrics {
    uint16_t advance;  // Font units.
    int8_t left;       // Bearings and sizes in strike pixels.
    int8_t top;
    uint8_t width;
    uint8_t height;
  };

  struct Bitmap {
    Metrics metrics;
    std::span<const uint8_t> pixels;  // Row-aligned, MSB first.
  };

  struct Variant {
    Glyph glyph;
    uint16_t advance;
  };

  struct AssemblyPart {
    Glyph glyph;
    uint16_t startConnector;
    uint16_t endConnector;
    uint16_t advance;
    bool extender;
  };

  struct Construction {
    std::array<Variant, 13> variants;
    std::array<AssemblyPart, 5> parts;
    int16_t italicCorrection;
    uint8_t variantCount;
    uint8_t partCount;
  };

  explicit MathFont(std::span<const uint8_t> asset);
  bool valid() const { return !asset_.empty(); }
  uint8_t strikePpem() const { return strikePpem_; }
  uint16_t unitsPerEm() const { return unitsPerEm_; }
  uint16_t glyphCount() const { return glyphCount_; }
  Glyph glyphForCodepoint(uint32_t codepoint) const;
  std::optional<Metrics> metrics(Glyph glyph);
  // Pixels remain valid until the next bitmap() call on this font instance.
  std::optional<Bitmap> bitmap(Glyph glyph);
  int16_t constant(Constant index) const;
  int16_t italicCorrection(Glyph glyph);
  std::optional<int16_t> topAccent(Glyph glyph);
  bool isExtendedShape(Glyph glyph) const;
  int16_t mathKern(Glyph glyph, Corner corner, int16_t height);
  // Results contain font units and are independent of the scratch block.
  bool construction(Glyph glyph, Direction direction, Construction& result);

 private:
  enum class Section : uint8_t {
    Cmap,
    Metrics,
    Bitmaps,
    Constants,
    Italics,
    Accents,
    Kern,
    Extended,
    Vertical,
    Horizontal,
    Count,
  };

  struct Block {
    const uint8_t* descriptor = nullptr;
    std::span<const uint8_t> payload;
    uint16_t first = 0;
    uint16_t count = 0;
  };

  static constexpr size_t MaxRawBlockBytes = 2048;
  static constexpr size_t MaxBitmapBytes = 192;
  static constexpr size_t MaxBlockRecords = 64;

  std::span<const uint8_t> section(Section id) const;
  Block findBlock(Section id, Glyph glyph) const;
  std::span<const uint8_t> readBlock(const Block& block);
  std::optional<int16_t> sparseValue(Section id, Glyph glyph);

  std::span<const uint8_t> asset_;
  uint8_t strikePpem_ = 0;
  uint16_t unitsPerEm_ = 0;
  uint16_t glyphCount_ = 0;
  const uint8_t* loadedBlock_ = nullptr;
  InflateReader inflate_;
  std::array<uint8_t, MaxRawBlockBytes + 1> raw_;
  std::array<uint8_t, MaxBitmapBytes> bitmap_;
};

}  // namespace mathml
