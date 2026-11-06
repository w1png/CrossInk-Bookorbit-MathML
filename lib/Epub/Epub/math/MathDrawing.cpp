#include "MathDrawing.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "MathFont.h"

namespace mathml {
namespace {

// EPUB page draws are serialized by RenderLock. The ~3.5 KiB decoder exceeds
// the render stack budget. Allocate it only when a math glyph needs drawing.
std::unique_ptr<MathFont> renderFont;
bool renderFontLoadFailed = false;

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

void releaseRenderWorkspace() {
  renderFont.reset();
  renderFontLoadFailed = false;
}

void renderDrawing(const Drawing& drawing, GfxRenderer& renderer, const int fontId, const int x, const int y,
                   const bool foregroundBlack) {
  if (!drawing.commands) return;

  const bool scanning = renderer.isFontCacheScanning();
  // The reader's text prewarm scan starts a new page render. After a failed
  // allocation wait for the next scan instead of retrying in each strip.
  if (scanning) renderFontLoadFailed = false;
  const bool drawsBinary =
      !scanning && (renderer.getRenderMode() == GfxRenderer::BW || renderer.grayPlanesAreAbsolute()) &&
      renderer.glyphIntersectsStrip(x, y, x + drawing.width - 1, y + drawing.ascent + drawing.descent - 1);
  if (!renderFont && (drawsBinary || renderFontLoadFailed) &&
      std::any_of(drawing.commands.get(), drawing.commands.get() + drawing.commandCount,
                  [](const DrawCommand& command) { return command.type == DrawType::Glyph; })) {
    if (!renderFontLoadFailed) {
      renderFont = makeUniqueNoThrow<MathFont>(bundledMathFontBytes());
      if (!renderFont || !renderFont->valid()) {
        LOG_ERR("MTH", "Could not open the math font for rendering (%u bytes)",
                static_cast<unsigned>(sizeof(MathFont)));
        renderFont.reset();
        renderFontLoadFailed = true;
      }
    }
    if (renderFontLoadFailed) {
      // Show a missing-equation box inside the reserved bounds. Rules or
      // prose without glyphs could change the meaning of the expression.
      if (drawsBinary) {
        const int size = std::min({12, static_cast<int>(drawing.width), drawing.ascent + drawing.descent});
        if (size > 0) renderer.drawRect(x, y, size, size, foregroundBlack);
      }
      return;
    }
  }
  const int textFontId = renderer.getFontIdForSize(fontId, drawing.fontSize);
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
        drawGlyph(*renderFont, renderer, command, x + command.x, y + command.y, foregroundBlack);
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
