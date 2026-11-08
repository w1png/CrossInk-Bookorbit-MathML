#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>

class GfxRenderer;

namespace mathml {

enum class DrawType : uint8_t { Glyph, Text, Rule, Fill };

// Coordinates are relative to the drawing's top-left corner. Glyphs use a
// baseline Y, X/Y pixels per em in x2/y2, and a glyph ID in value. Text uses a
// top Y, a NUL-terminated text offset in value, and reader font flags in style.
// Rules use both endpoints and a thickness in value. Fills use x2/y2 as width
// and height, and style selects foreground (0) or background (1) ink.
struct DrawCommand {
  int16_t x = 0;
  int16_t y = 0;
  int16_t x2 = 0;
  int16_t y2 = 0;
  uint16_t value = 0;
  DrawType type = DrawType::Glyph;
  uint8_t style = 0;
};
static_assert(sizeof(DrawCommand) == 12);
static_assert(std::is_trivially_copyable_v<DrawCommand>);

struct Drawing {
  static constexpr uint16_t MAX_COMMANDS = 512;
  static constexpr uint16_t MAX_TEXT_BYTES = 2048;
  static constexpr uint16_t MAX_DIMENSION = 4095;

  uint16_t width = 0;
  uint16_t ascent = 0;
  uint16_t descent = 0;
  uint16_t commandCount = 0;
  uint16_t textBytes = 0;
  uint8_t fontSize = 0;  // Reader font override in pixels, or zero for the current reader font.
  std::unique_ptr<DrawCommand[]> commands;
  std::unique_ptr<char[]> text;
};

void renderDrawing(const Drawing& drawing, GfxRenderer& renderer, int fontId, int x, int y, bool foregroundBlack);
// Call under the same render lock as page drawing to release reader scratch.
void releaseRenderWorkspace();

}  // namespace mathml
