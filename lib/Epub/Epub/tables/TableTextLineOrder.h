#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace TableTextLineOrder {

// Math can give the cells of a row different line heights. Merge their
// already ordered lines by the visible text Y, using column order for ties.
template <size_t MaxCells, typename Row, typename Visitor>
bool forEachPositionedCellLineInVisualOrder(const Row& row, const int defaultLineHeight, Visitor&& visitor) {
  std::array<size_t, MaxCells> nextLine{};
  std::array<int, MaxCells> nextTop{};
  const size_t cellCount = std::min(row.cells.size(), MaxCells);
  for (;;) {
    size_t cell = cellCount;
    int textY = std::numeric_limits<int>::max();
    for (size_t candidate = 0; candidate < cellCount; ++candidate) {
      const auto& lines = row.cells[candidate].lines;
      if (nextLine[candidate] >= lines.size()) continue;
      const int candidateY = nextTop[candidate] + lines[nextLine[candidate]].textTop();
      if (candidateY < textY) {
        cell = candidate;
        textY = candidateY;
      }
    }
    if (cell == cellCount) return true;
    const size_t index = nextLine[cell]++;
    if (!visitor(cell, index, textY)) return false;
    nextTop[cell] += row.cells[cell].lines[index].height(defaultLineHeight);
  }
}

}  // namespace TableTextLineOrder
