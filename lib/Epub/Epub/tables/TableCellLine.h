#pragma once

#include <memory>
#include <utility>

#include "../InlineObjectToken.h"

class PageMath;
class TextBlock;

struct TableCellMathPlacement {
  InlineObjectPlacement placement{};  // Used only until the parser places the drawing.
  std::unique_ptr<PageMath> drawing;
};

struct TableCellMath {
  static constexpr uint8_t MAX_OBJECTS = 64;
  InlineLineMetrics metrics{};
  uint8_t count = 0;
  std::unique_ptr<TableCellMathPlacement[]> placements;
};

struct TableCellLine {
  std::shared_ptr<TextBlock> text;
  std::unique_ptr<TableCellMath> math;

  explicit TableCellLine(std::shared_ptr<TextBlock> text) : text(std::move(text)) {}
  int height(int defaultHeight) const { return math ? math->metrics.height : defaultHeight; }
  int textTop() const { return math ? math->metrics.textTop : 0; }
  bool setInlineMath(std::span<const InlineObjectPlacement> objects, int textHeight, int textBaseline);
};
