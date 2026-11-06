#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "MathDrawing.h"

namespace mathml {

constexpr uint16_t NO_NODE = UINT16_MAX;

// The parser owns the tree and the strings until layout returns. Attributes
// are consecutive NUL-terminated name and value pairs, with no extra sentinel.
struct Node {
  uint16_t firstChild = NO_NODE;
  uint16_t nextSibling = NO_NODE;
  std::string_view name;
  std::string_view attributes;
  std::string_view text;
};

bool layout(std::span<const Node> nodes, GfxRenderer& renderer, int fontId, uint16_t maxWidth, uint16_t maxHeight,
            uint8_t fontSize, bool display, Drawing& result);

}  // namespace mathml
