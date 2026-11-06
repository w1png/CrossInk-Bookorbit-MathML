#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/blocks/TextBlock.h"
#include "TableCellLine.h"

class PageMath {};

struct TableFragmentCell {
  static constexpr uint8_t MAX_SERIALIZED_LINES = 64;
  bool isHeader = false;
  uint8_t colSpan = 1;
  std::vector<TableCellLine> lines;

  int contentHeight(const int lineHeight) const {
    if (lines.empty()) return lineHeight;
    int height = 0;
    for (const auto& line : lines) height += line.height(lineHeight);
    return height;
  }
};

struct TableFragmentRow {
  static constexpr uint8_t MAX_SERIALIZED_CELLS = 8;
  uint16_t height = 0;
  bool headerSeparator = false;
  std::vector<TableFragmentCell> cells;
};

class Page {
 public:
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;
};
