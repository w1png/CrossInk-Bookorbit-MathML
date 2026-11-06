#include "MathDrawing.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "MathFont.h"

namespace mathml {
namespace {

int scaleMetric(const int value, const int ppem, const int strikePpem) {
  const int scaled = value * ppem;
  return (scaled + (scaled < 0 ? -strikePpem / 2 : strikePpem / 2)) / strikePpem;
}

bool hasInk(const std::span<const uint8_t> pixels, const int stride, const int x0, const int y0, const int x1,
            const int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (pixels[y * stride + x / 8] & (0x80 >> (x % 8))) return true;
    }
  }
  return false;
}

void drawGlyph(MathFont& font, GfxRenderer& renderer, const DrawCommand& command, const int x, const int baseline,
               const bool black) {
  if (command.x2 <= 0 || command.y2 <= 0) return;
  const auto bitmap = font.bitmap(command.value);
  if (!bitmap || bitmap->pixels.empty()) return;

  const auto& metrics = bitmap->metrics;
  const int strikePpem = font.strikePpem();
  const int left = x + scaleMetric(metrics.left, command.x2, strikePpem);
  const int top = baseline - scaleMetric(metrics.top, command.y2, strikePpem);
  const int width = std::max(1, scaleMetric(metrics.width, command.x2, strikePpem));
  const int height = std::max(1, scaleMetric(metrics.height, command.y2, strikePpem));
  const int stride = (metrics.width + 7) / 8;

  if (!renderer.glyphIntersectsStrip(left, top, left + width - 1, top + height - 1)) return;
  const int firstColumn = std::max(0, -left);
  const int lastColumn = std::min(width, renderer.getScreenWidth() - left);
  const int firstRow = std::max(0, -top);
  const int lastRow = std::min(height, renderer.getScreenHeight() - top);
  for (int row = firstRow; row < lastRow; ++row) {
    const int sourceY = row * metrics.height / height;
    const int sourceBottom = std::max(sourceY + 1, (row + 1) * metrics.height / height);
    for (int column = firstColumn; column < lastColumn; ++column) {
      const int sourceX = column * metrics.width / width;
      const int sourceRight = std::max(sourceX + 1, (column + 1) * metrics.width / width);
      // Preserve thin strokes when several source pixels share a destination.
      if (hasInk(bitmap->pixels, stride, sourceX, sourceY, sourceRight, sourceBottom)) {
        renderer.drawPixel(left + column, top + row, black);
      }
    }
  }
}

void drawRule(GfxRenderer& renderer, const DrawCommand& command, const int x, const int y, const bool black) {
  const int firstY = y + command.y;
  const int lastY = y + command.y2;
  if (command.x == command.x2) {
    renderer.fillRect(x + command.x, std::min(firstY, lastY), command.value,
                      std::max(firstY, lastY) - std::min(firstY, lastY) + 1, black);
    return;
  }
  renderer.drawLine(x + command.x, firstY, x + command.x2, lastY, command.value, black);
}

}  // namespace

void renderDrawing(const Drawing& drawing, GfxRenderer& renderer, const int fontId, const int x, const int y,
                   const bool foregroundBlack) {
  if (!drawing.commands) return;

  const bool drawsBinary = !renderer.isFontCacheScanning() &&
                           (renderer.getRenderMode() == GfxRenderer::BW || renderer.grayPlanesAreAbsolute()) &&
                           renderer.glyphIntersectsStrip(x, y, x + drawing.width - 1, y + drawing.ascent + drawing.descent - 1);
  const int textFontId = renderer.getFontIdForSize(fontId, drawing.fontSize);
  // The decoder exceeds the render task's stack budget. One fallible
  // allocation supplies scratch for every glyph in this drawing.
  std::unique_ptr<MathFont> mathFont;
  bool triedFont = false;
  for (uint16_t index = 0; index < drawing.commandCount; ++index) {
    const auto& command = drawing.commands[index];
    if (command.type == DrawType::Text) {
      if (drawing.text && command.value < drawing.textBytes) {
        const char* text = drawing.text.get() + command.value;
        if (std::memchr(text, '\0', drawing.textBytes - command.value)) {
          renderer.drawText(textFontId, x + command.x, y + command.y, text, foregroundBlack,
                            static_cast<EpdFontFamily::Style>(command.style));
        }
      }
      continue;
    }
    if (!drawsBinary) continue;

    switch (command.type) {
      case DrawType::Glyph:
        if (!triedFont) {
          triedFont = true;
          mathFont = makeUniqueNoThrow<MathFont>(bundledMathFontBytes());
          if (!mathFont || !mathFont->valid()) LOG_ERR("MTH", "Could not open the math font for rendering");
        }
        if (mathFont && mathFont->valid()) {
          drawGlyph(*mathFont, renderer, command, x + command.x, y + command.y, foregroundBlack);
        }
        break;
      case DrawType::Rule:
        drawRule(renderer, command, x, y, foregroundBlack);
        break;
      case DrawType::Fill:
        renderer.fillRect(x + command.x, y + command.y, command.x2, command.y2,
                          command.style == 0 ? foregroundBlack : !foregroundBlack);
        break;
      case DrawType::Text:
        break;
    }
  }
}

}  // namespace mathml
