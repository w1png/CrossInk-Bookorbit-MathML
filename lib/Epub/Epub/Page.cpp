#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdlib>

#include "tables/TableColumnLayout.h"
#include "tables/TableTextLineOrder.h"

namespace {

constexpr uint16_t MAX_PAGE_ELEMENTS = 1024;
constexpr uint8_t MAX_TABLE_ROWS_PER_FRAGMENT = 64;
constexpr uint8_t MAX_TABLE_CELLS_PER_ROW = 8;
constexpr uint8_t MAX_TABLE_LINES_PER_CELL = 64;
static_assert(TableFragmentCell::MAX_SERIALIZED_LINES == MAX_TABLE_LINES_PER_CELL);
static_assert(TableFragmentRow::MAX_SERIALIZED_CELLS == MAX_TABLE_CELLS_PER_ROW);
static_assert(PageTableFragment::MAX_SERIALIZED_ROWS == MAX_TABLE_ROWS_PER_FRAGMENT);

uint16_t tableSelectionForLine(const size_t elementIndex, const uint8_t logicalColumn) {
  if (elementIndex >= MAX_PAGE_ELEMENTS || logicalColumn >= MAX_TABLE_CELLS_PER_ROW) {
    return UINT16_MAX;
  }
  return static_cast<uint16_t>(elementIndex * MAX_TABLE_CELLS_PER_ROW + logicalColumn);
}

bool validMathMetadata(const mathml::Drawing& drawing) {
  return drawing.width <= mathml::Drawing::MAX_DIMENSION &&
         static_cast<int>(drawing.ascent) + drawing.descent <= mathml::Drawing::MAX_DIMENSION &&
         drawing.commandCount <= mathml::Drawing::MAX_COMMANDS && drawing.textBytes <= mathml::Drawing::MAX_TEXT_BYTES;
}

bool validMathCommands(const mathml::Drawing& drawing) {
  if ((drawing.commandCount && !drawing.commands) ||
      (drawing.textBytes && (!drawing.text || drawing.text[drawing.textBytes - 1] != '\0'))) {
    return false;
  }
  constexpr int limit = mathml::Drawing::MAX_DIMENSION;
  for (uint16_t i = 0; i < drawing.commandCount; ++i) {
    const auto& command = drawing.commands[i];
    if (std::abs(static_cast<int>(command.x)) > limit || std::abs(static_cast<int>(command.y)) > limit) return false;
    switch (command.type) {
      case mathml::DrawType::Glyph:
      case mathml::DrawType::Fill:
        if (command.x2 <= 0 || command.x2 > limit || command.y2 <= 0 || command.y2 > limit) return false;
        if (command.type == mathml::DrawType::Fill && command.style > 1) return false;
        break;
      case mathml::DrawType::Text:
        if (command.value >= drawing.textBytes || (command.value > 0 && drawing.text[command.value - 1] != '\0')) {
          return false;
        }
        break;
      case mathml::DrawType::Rule:
        if (std::abs(static_cast<int>(command.x2)) > limit || std::abs(static_cast<int>(command.y2)) > limit ||
            command.value == 0 || command.value > limit) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, const bool foregroundBlack,
                                Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const bool foregroundBlack) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

bool PageLine::serialize(FsFile& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Serialization failed: could not write PageLine coordinates");
    return false;
  }

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageLine coordinates");
    return nullptr;
  }

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: PageLine text block was invalid");
    return nullptr;
  }

  auto* pageLine = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!pageLine) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(pageLine);
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                       const bool foregroundBlack) {
  (void)fontId;
  // Images don't use fontId for text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset,
                                  const bool foregroundBlack) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

bool PageImage::serialize(FsFile& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(inlineImage))) {
    LOG_ERR("PGE", "Serialization failed: could not write PageImage coordinates");
    return false;
  }

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  uint8_t inlineFlag;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, inlineFlag) || inlineFlag > 1) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageImage coordinates");
    return nullptr;
  }

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: PageImage block was invalid");
    return nullptr;
  }

  auto* pageImage = new (std::nothrow) PageImage(std::move(ib), xPos, yPos, inlineFlag != 0);
  if (!pageImage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return std::unique_ptr<PageImage>(pageImage);
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                                const bool foregroundBlack) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness,
                    foregroundBlack);
}

bool PageHorizontalRule::serialize(FsFile& file) {
  return serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos) &&
         serialization::tryWritePod(file, width) && serialization::tryWritePod(file, thickness);
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, thickness)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageHorizontalRule metadata");
    return nullptr;
  }

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

void PageMath::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const bool foregroundBlack) {
  mathml::renderDrawing(drawing, renderer, fontId, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

bool PageMath::serialize(FsFile& file) {
  if (!validMathMetadata(drawing) || !validMathCommands(drawing)) {
    LOG_ERR("PGE", "Serialization failed: invalid MathML drawing");
    return false;
  }
  const size_t commandBytes = drawing.commandCount * sizeof(mathml::DrawCommand);
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, drawing.width) || !serialization::tryWritePod(file, drawing.ascent) ||
      !serialization::tryWritePod(file, drawing.descent) || !serialization::tryWritePod(file, drawing.commandCount) ||
      !serialization::tryWritePod(file, drawing.textBytes) || !serialization::tryWritePod(file, drawing.fontSize) ||
      (commandBytes > 0 &&
       file.write(reinterpret_cast<const uint8_t*>(drawing.commands.get()), commandBytes) != commandBytes) ||
      (drawing.textBytes > 0 &&
       file.write(reinterpret_cast<const uint8_t*>(drawing.text.get()), drawing.textBytes) != drawing.textBytes)) {
    LOG_ERR("PGE", "Serialization failed: could not write MathML drawing");
    return false;
  }
  return true;
}

std::unique_ptr<PageMath> PageMath::deserialize(FsFile& file) {
  int16_t x = 0;
  int16_t y = 0;
  mathml::Drawing drawing;
  if (!serialization::tryReadPod(file, x) || !serialization::tryReadPod(file, y) ||
      !serialization::tryReadPod(file, drawing.width) || !serialization::tryReadPod(file, drawing.ascent) ||
      !serialization::tryReadPod(file, drawing.descent) || !serialization::tryReadPod(file, drawing.commandCount) ||
      !serialization::tryReadPod(file, drawing.textBytes) || !serialization::tryReadPod(file, drawing.fontSize) ||
      !validMathMetadata(drawing)) {
    LOG_ERR("PGE", "Deserialization failed: invalid MathML drawing metadata");
    return nullptr;
  }

  const int commandBytes = drawing.commandCount * sizeof(mathml::DrawCommand);
  if (file.available() < commandBytes + drawing.textBytes) {
    LOG_ERR("PGE", "Deserialization failed: truncated MathML drawing");
    return nullptr;
  }
  // The page owns the draw list across renders. Size each allocation to the
  // cached data (at most 6 KiB of commands and 2 KiB of prose per equation).
  if (drawing.commandCount > 0) {
    drawing.commands = makeUniqueNoThrow<mathml::DrawCommand[]>(drawing.commandCount);
    if (!drawing.commands || file.read(drawing.commands.get(), commandBytes) != commandBytes) {
      LOG_ERR("PGE", "Deserialization failed: could not read %u MathML commands", drawing.commandCount);
      return nullptr;
    }
  }
  if (drawing.textBytes > 0) {
    drawing.text = makeUniqueNoThrow<char[]>(drawing.textBytes);
    if (!drawing.text || file.read(drawing.text.get(), drawing.textBytes) != drawing.textBytes) {
      LOG_ERR("PGE", "Deserialization failed: could not read %u MathML prose bytes", drawing.textBytes);
      return nullptr;
    }
  }
  if (!validMathCommands(drawing)) {
    LOG_ERR("PGE", "Deserialization failed: invalid MathML drawing commands");
    return nullptr;
  }

  auto result = makeUniqueNoThrow<PageMath>(std::move(drawing), x, y);
  if (!result) LOG_ERR("PGE", "Deserialization failed: could not allocate PageMath");
  return result;
}

namespace {

bool serializeTableCellLine(FsFile& file, const TableCellLine& line) {
  const auto& math = line.math;
  const uint8_t count = math ? math->count : 0;
  if (!line.text || !line.text->serialize(file) || !serialization::tryWritePod(file, count)) return false;
  if (count == 0) return true;
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(math->metrics.height)) ||
      !serialization::tryWritePod(file, static_cast<uint16_t>(math->metrics.textTop)) ||
      !serialization::tryWritePod(file, static_cast<uint16_t>(math->metrics.baseline))) {
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    const auto& drawing = math->placements[i].drawing;
    if (!drawing || !drawing->serialize(file)) return false;
  }
  return true;
}

bool deserializeTableCellLineMath(FsFile& file, TableCellLine& line) {
  uint8_t count = 0;
  if (!serialization::tryReadPod(file, count) || count > TableCellMath::MAX_OBJECTS) return false;
  if (count == 0) return true;

  uint16_t height = 0;
  uint16_t textTop = 0;
  uint16_t baseline = 0;
  if (!serialization::tryReadPod(file, height) || !serialization::tryReadPod(file, textTop) ||
      !serialization::tryReadPod(file, baseline) || height == 0 || textTop > baseline || baseline > height) {
    return false;
  }
  auto sidecar = makeUniqueNoThrow<TableCellMath>();
  if (!sidecar || !(sidecar->placements = makeUniqueNoThrow<TableCellMathPlacement[]>(count))) return false;
  sidecar->count = count;
  sidecar->metrics = {height, textTop, baseline};
  for (uint8_t i = 0; i < count; ++i) {
    auto drawing = PageMath::deserialize(file);
    if (!drawing || drawing->yPos < 0 || drawing->yPos + drawing->getDrawing().ascent != baseline ||
        baseline + drawing->getDrawing().descent > height) {
      return false;
    }
    sidecar->placements[i].drawing = std::move(drawing);
  }
  line.math = std::move(sidecar);
  return true;
}

}  // namespace

bool TableFragmentCell::serialize(FsFile& file) const {
  if (colSpan == 0 || colSpan > MAX_TABLE_CELLS_PER_ROW || lines.size() > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Serialization failed: invalid cell span/line count (span=%u lines=%u)", colSpan,
            static_cast<uint32_t>(lines.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, isHeader) || !serialization::tryWritePod(file, colSpan) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(lines.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table cell header");
    return false;
  }
  for (const auto& line : lines) {
    if (!serializeTableCellLine(file, line)) {
      LOG_ERR("PTB", "Serialization failed: invalid table cell line");
      return false;
    }
  }
  return true;
}

bool TableFragmentCell::deserialize(FsFile& file, TableFragmentCell& outCell) {
  uint8_t lineCount = 0;
  if (!serialization::tryReadPod(file, outCell.isHeader) || !serialization::tryReadPod(file, outCell.colSpan) ||
      !serialization::tryReadPod(file, lineCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table cell metadata");
    return false;
  }
  if (outCell.colSpan == 0 || outCell.colSpan > MAX_TABLE_CELLS_PER_ROW || lineCount > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Deserialization failed: invalid cell span/line count (span=%u lines=%u)", outCell.colSpan,
            lineCount);
    return false;
  }

  outCell.lines.clear();
  outCell.lines.reserve(lineCount);
  for (uint8_t i = 0; i < lineCount; i++) {
    auto text = TextBlock::deserialize(file);
    if (!text) {
      LOG_ERR("PTB", "Deserialization failed: invalid table cell line");
      return false;
    }
    auto& line = outCell.lines.emplace_back(std::move(text));
    if (!deserializeTableCellLineMath(file, line)) {
      LOG_ERR("PTB", "Deserialization failed: invalid table cell MathML");
      return false;
    }
  }
  return true;
}

bool TableFragmentRow::serialize(FsFile& file) const {
  if (cells.size() > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Serialization failed: row cell count %u exceeds maximum", static_cast<uint32_t>(cells.size()));
    return false;
  }
  uint8_t logicalColumns = 0;
  for (const auto& cell : cells) {
    if (cell.colSpan == 0 || static_cast<uint8_t>(logicalColumns + cell.colSpan) > MAX_TABLE_CELLS_PER_ROW) {
      LOG_ERR("PTB", "Serialization failed: row colspan exceeds maximum");
      return false;
    }
    logicalColumns = static_cast<uint8_t>(logicalColumns + cell.colSpan);
  }

  if (!serialization::tryWritePod(file, height) || !serialization::tryWritePod(file, headerSeparator) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(cells.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table row metadata");
    return false;
  }
  for (const auto& cell : cells) {
    if (!cell.serialize(file)) {
      return false;
    }
  }
  return true;
}

bool TableFragmentRow::deserialize(FsFile& file, TableFragmentRow& outRow) {
  uint8_t cellCount = 0;
  if (!serialization::tryReadPod(file, outRow.height) || !serialization::tryReadPod(file, outRow.headerSeparator) ||
      !serialization::tryReadPod(file, cellCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table row metadata");
    return false;
  }
  if (cellCount > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Deserialization failed: row cell count %u exceeds maximum", cellCount);
    return false;
  }

  outRow.cells.clear();
  outRow.cells.reserve(cellCount);
  uint8_t logicalColumns = 0;
  for (uint8_t i = 0; i < cellCount; i++) {
    TableFragmentCell cell;
    if (!TableFragmentCell::deserialize(file, cell)) {
      return false;
    }
    if (static_cast<uint8_t>(logicalColumns + cell.colSpan) > MAX_TABLE_CELLS_PER_ROW) {
      LOG_ERR("PTB", "Deserialization failed: row colspan exceeds maximum");
      return false;
    }
    logicalColumns = static_cast<uint8_t>(logicalColumns + cell.colSpan);
    outRow.cells.push_back(std::move(cell));
  }
  return true;
}

uint16_t PageTableFragment::getHeight() const {
  uint16_t total = 1;  // Bottom border.
  for (const auto& row : rows) {
    total = static_cast<uint16_t>(total + row.height);
  }
  return total;
}

void PageTableFragment::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                               const bool foregroundBlack) {
  if (columnCount == 0 || rows.empty() || width < 2) {
    return;
  }

  const int drawX = xPos + xOffset;
  const int drawY = yPos + yOffset;
  const uint16_t totalHeight = getHeight();

  std::vector<int16_t> columnStarts(columnCount + 1);
  for (uint8_t i = 0; i < columnCount; i++) {
    columnStarts[i] = static_cast<int16_t>(TableColumnLayout::columnStart(width, columnCount, i));
  }
  // Text clips use a half-open right edge; keep the final cell's full width
  // available while the border itself remains one pixel inside the table.
  columnStarts[columnCount] = static_cast<int16_t>(TableColumnLayout::columnStart(width, columnCount, columnCount));

  renderer.drawRect(drawX, drawY, width, totalHeight, foregroundBlack);

  int currentY = 0;
  for (size_t rowIndex = 0; rowIndex < rows.size(); rowIndex++) {
    const auto& row = rows[rowIndex];

    uint8_t logicalColumn = 0;

    for (size_t colIndex = 0; colIndex < row.cells.size() && colIndex < columnCount; colIndex++) {
      if (logicalColumn >= columnCount) break;
      const auto& cell = row.cells[colIndex];
      const uint8_t span =
          std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan, static_cast<uint8_t>(columnCount - logicalColumn));
      const int cellTextX = drawX + columnStarts[logicalColumn] + cellPadding;
      const int cellTextY = drawY + currentY + cellPadding;
      const int cellTextWidth = columnStarts[logicalColumn + span] - columnStarts[logicalColumn] - cellPadding * 2;
      const int cellTextHeight = row.height - cellPadding * 2;

      renderer.beginTextClip(cellTextX, cellTextY, cellTextWidth, cellTextHeight);
      int lineY = cellTextY;
      for (const auto& line : cell.lines) {
        line.text->render(renderer, fontId, cellTextX, lineY + line.textTop(), foregroundBlack);
        if (line.math) {
          for (uint8_t i = 0; i < line.math->count; ++i) {
            const auto& drawing = line.math->placements[i].drawing;
            if (drawing) drawing->render(renderer, fontId, cellTextX, lineY, foregroundBlack);
          }
        }
        lineY += line.height(lineHeight);
      }
      renderer.endTextClip();
      logicalColumn = static_cast<uint8_t>(logicalColumn + span);
    }

    // Colspan cells own the interior vertical boundaries they cover. Draw
    // only boundaries that are present in this row, preserving the existing
    // one-column grid for ordinary cells.
    logicalColumn = 0;
    for (const auto& cell : row.cells) {
      if (logicalColumn >= columnCount) break;
      const uint8_t span =
          std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan, static_cast<uint8_t>(columnCount - logicalColumn));
      if (logicalColumn + span < columnCount) {
        const int x = drawX + columnStarts[logicalColumn + span];
        renderer.drawLine(x, drawY + currentY, x, drawY + currentY + row.height - 1, foregroundBlack);
      }
      logicalColumn = static_cast<uint8_t>(logicalColumn + span);
      if (logicalColumn >= columnCount) break;
    }

    currentY += row.height;
    if (rowIndex + 1 < rows.size()) {
      const int lineWidth = row.headerSeparator ? 2 : 1;
      renderer.drawLine(drawX, drawY + currentY, drawX + width - 1, drawY + currentY, lineWidth, foregroundBlack);
    }
  }
}

bool Page::forEachTextLine(const PageTextLineVisitor visitor, void* context) const {
  if (!visitor) return false;

  for (size_t elementIndex = 0; elementIndex < elements.size(); ++elementIndex) {
    const auto& element = elements[elementIndex];
    if (!element) continue;

    if (element->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock() && !visitor({line.getBlock().get(), line.xPos, line.yPos, 0, 0, 0, 0,
                                       line.getBlock()->getBlockStyle().lineHeight},
                                      context)) {
        return false;
      }
      continue;
    }

    if (element->getTag() != TAG_PageTableFragment) continue;
    const auto& fragment = static_cast<const PageTableFragment&>(*element);
    if (fragment.columnCount == 0 || fragment.rows.empty() || fragment.width < 2) continue;

    int currentY = 0;
    for (const auto& row : fragment.rows) {
      const bool visited = TableTextLineOrder::forEachPositionedCellLineInVisualOrder<MAX_TABLE_CELLS_PER_ROW>(
          row, fragment.lineHeight, [&](const size_t cellIndex, const size_t lineIndex, const int textY) {
            uint8_t logicalColumn = 0;
            for (size_t precedingCell = 0; precedingCell < cellIndex; ++precedingCell) {
              if (logicalColumn >= fragment.columnCount) return true;
              const auto& cell = row.cells[precedingCell];
              logicalColumn = static_cast<uint8_t>(
                  logicalColumn + std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan,
                                                    static_cast<uint8_t>(fragment.columnCount - logicalColumn)));
            }
            if (logicalColumn >= fragment.columnCount) return true;

            const auto& cell = row.cells[cellIndex];
            const auto& text = cell.lines[lineIndex].text;
            if (!text || text->wordCount() == 0) return true;
            const uint8_t span = std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan,
                                                   static_cast<uint8_t>(fragment.columnCount - logicalColumn));
            const int cellX = fragment.xPos +
                              TableColumnLayout::columnStart(fragment.width, fragment.columnCount, logicalColumn) +
                              fragment.cellPadding;
            const int cellY = fragment.yPos + currentY + fragment.cellPadding;
            const int cellWidth = TableColumnLayout::innerWidth(fragment.width, fragment.columnCount, logicalColumn,
                                                                span, fragment.cellPadding);
            const int cellHeight = std::max(0, static_cast<int>(row.height) - fragment.cellPadding * 2);
            const PageTextLine line{
                text.get(), cellX,      cellY + textY,       cellX, cellY,
                cellWidth,  cellHeight, fragment.lineHeight, true,  tableSelectionForLine(elementIndex, logicalColumn)};
            return visitor(line, context);
          });
      if (!visited) return false;
      currentY += row.height;
    }
  }
  return true;
}

bool PageTableFragment::serialize(FsFile& file) {
  if (rows.size() > MAX_TABLE_ROWS_PER_FRAGMENT) {
    LOG_ERR("PTB", "Serialization failed: fragment row count %u exceeds maximum", static_cast<uint32_t>(rows.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, width) || !serialization::tryWritePod(file, columnCount) ||
      !serialization::tryWritePod(file, cellPadding) || !serialization::tryWritePod(file, lineHeight) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(rows.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write fragment metadata");
    return false;
  }
  for (const auto& row : rows) {
    if (!row.serialize(file)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<PageTableFragment> PageTableFragment::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t columnCount = 0;
  uint8_t cellPadding = 0;
  uint16_t lineHeight = 0;
  uint8_t rowCount = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, columnCount) ||
      !serialization::tryReadPod(file, cellPadding) || !serialization::tryReadPod(file, lineHeight) ||
      !serialization::tryReadPod(file, rowCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated fragment metadata");
    return nullptr;
  }

  if (rowCount == 0 || rowCount > MAX_TABLE_ROWS_PER_FRAGMENT || columnCount == 0 ||
      columnCount > MAX_TABLE_CELLS_PER_ROW || width < 2 || lineHeight == 0) {
    LOG_ERR("PTB", "Deserialization failed: invalid fragment metadata (rows=%u cols=%u width=%u lineHeight=%u)",
            rowCount, columnCount, width, lineHeight);
    return nullptr;
  }

  std::vector<TableFragmentRow> rows;
  rows.reserve(rowCount);
  for (uint8_t i = 0; i < rowCount; i++) {
    TableFragmentRow row;
    if (!TableFragmentRow::deserialize(file, row)) {
      return nullptr;
    }
    rows.push_back(std::move(row));
  }

  auto* fragment =
      new (std::nothrow) PageTableFragment(width, columnCount, cellPadding, lineHeight, std::move(rows), xPos, yPos);
  if (!fragment) {
    LOG_ERR("PTB", "Deserialization failed: could not allocate PageTableFragment");
    return nullptr;
  }
  return std::unique_ptr<PageTableFragment>(fragment);
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const bool foregroundBlack) const {
  renderText(renderer, fontId, xOffset, yOffset, foregroundBlack);
  renderImages(renderer, fontId, xOffset, yOffset, foregroundBlack);
}

void Page::renderText(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const bool foregroundBlack) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, foregroundBlack,
                             [](const PageElement& element) { return element.getTag() != TAG_PageImage; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                        const bool foregroundBlack) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, foregroundBlack,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                                       const bool foregroundBlack, const bool renderCachedImages) const {
  renderText(renderer, fontId, xOffset, yOffset, foregroundBlack);
  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageImage) {
      continue;
    }
    auto& pageImage = static_cast<PageImage&>(*element);
    if (!renderCachedImages || pageImage.getImageBlock().needsDecode()) {
      pageImage.renderPlaceholder(renderer, xOffset, yOffset, foregroundBlack);
    } else {
      pageImage.render(renderer, fontId, xOffset, yOffset, foregroundBlack);
    }
  }
}

uint16_t Page::imageEstimateUnits(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  bool hasImage = false;
  bool hasBlockImage = false;
  bool hasReadableContent = false;
  uint64_t imageArea = 0;
  for (const auto& element : elements) {
    switch (element->getTag()) {
      case TAG_PageImage: {
        hasImage = true;
        const auto& pageImage = static_cast<const PageImage&>(*element);
        const auto& image = pageImage.getImageBlock();
        hasBlockImage = hasBlockImage || !pageImage.isInlineImage();
        if (viewportWidth > 0 && viewportHeight > 0) {
          const int imageTop = std::max(0, static_cast<int>(element->yPos));
          const int imageBottom =
              std::min(static_cast<int>(viewportHeight),
                       static_cast<int>(element->yPos) + std::max(0, static_cast<int>(image.getHeight())));
          if (imageBottom > imageTop) {
            const uint32_t visibleHeight = static_cast<uint32_t>(imageBottom - imageTop);
            // Inline icons occupy only part of a text line. Protect their
            // approximate area instead of counting every icon as a full-width
            // strip, which skews estimates for image-rich paragraphs.
            const uint32_t countedWidth =
                pageImage.isInlineImage() ? static_cast<uint32_t>(std::clamp<int>(image.getWidth(), 0, viewportWidth))
                                          : viewportWidth;
            imageArea += static_cast<uint64_t>(visibleHeight) * countedWidth;
          }
        }
        break;
      }
      case TAG_PageLine:
      case TAG_PageTableFragment:
      case TAG_PageMath:
        hasReadableContent = true;
        break;
      case TAG_PageHorizontalRule:
        break;
    }
  }

  if (!hasImage) return 0;
  if ((!hasReadableContent && hasBlockImage) || viewportWidth == 0 || viewportHeight == 0)
    return PageCountEstimator::kUnitsPerPage;

  const uint64_t units =
      (imageArea * PageCountEstimator::kUnitsPerPage) / (static_cast<uint64_t>(viewportWidth) * viewportHeight);
  return static_cast<uint16_t>(std::min<uint64_t>(PageCountEstimator::kUnitsPerPage, units));
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  if (elements.size() > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Serialization failed: element count %u exceeds maximum", static_cast<uint32_t>(elements.size()));
    return false;
  }
  if (!serialization::tryWritePod(file, count)) {
    LOG_ERR("PGE", "Serialization failed: could not write element count");
    return false;
  }

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    if (!serialization::tryWritePod(file, static_cast<uint8_t>(el->getTag()))) {
      LOG_ERR("PGE", "Serialization failed: could not write element tag");
      return false;
    }

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  if (!serialization::tryWritePod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to write footnote count");
    return false;
  }
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href) || !serialization::tryWritePod(file, fn.linkId)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  const uint8_t markerCount = std::min<uint8_t>(publisherPageMarkers.size(), MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE);
  if (!serialization::tryWritePod(file, markerCount)) {
    LOG_ERR("PGE", "Failed to write publisher page marker count");
    return false;
  }
  for (uint8_t i = 0; i < markerCount; i++) {
    const auto& marker = publisherPageMarkers[i];
    if (!serialization::tryWritePod(file, marker.yPos) ||
        file.write(marker.label, sizeof(marker.label)) != sizeof(marker.label)) {
      LOG_ERR("PGE", "Failed to write publisher page marker");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto* rawPage = new (std::nothrow) Page();
  if (!rawPage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }
  auto page = std::unique_ptr<Page>(rawPage);

  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    LOG_ERR("PGE", "Deserialization failed: could not read element count");
    return nullptr;
  }
  if (count > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Deserialization failed: element count %u exceeds maximum", count);
    return nullptr;
  }
  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    if (!serialization::tryReadPod(file, tag)) {
      LOG_ERR("PGE", "Deserialization failed: truncated element tag");
      return nullptr;
    }

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageTableFragment) {
      auto fragment = PageTableFragment::deserialize(file);
      if (!fragment) {
        return nullptr;
      }
      page->elements.push_back(std::move(fragment));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else if (tag == TAG_PageMath) {
      auto math = PageMath::deserialize(file);
      if (!math) {
        return nullptr;
      }
      page->elements.push_back(std::move(math));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  if (!serialization::tryReadPod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to read footnote count");
    return nullptr;
  }
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href) ||
        !serialization::tryReadPod(file, entry.linkId)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  uint8_t markerCount;
  if (!serialization::tryReadPod(file, markerCount)) {
    LOG_ERR("PGE", "Failed to read publisher page marker count");
    return nullptr;
  }
  if (markerCount > MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid publisher page marker count %u", markerCount);
    return nullptr;
  }
  page->publisherPageMarkers.reserve(markerCount);
  for (uint8_t i = 0; i < markerCount; i++) {
    Page::PublisherPageMarker marker;
    if (!serialization::tryReadPod(file, marker.yPos) ||
        file.read(marker.label, sizeof(marker.label)) != sizeof(marker.label)) {
      LOG_ERR("PGE", "Failed to read publisher page marker %u", i);
      return nullptr;
    }
    marker.label[sizeof(marker.label) - 1] = '\0';
    page->publisherPageMarkers.push_back(marker);
  }

  return page;
}

void Page::prepareImageCaches() const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).getImageBlock().prepareCache();
    }
  }
}
