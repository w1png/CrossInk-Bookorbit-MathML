#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

void ImageBlock::render(GfxRenderer&, int, int, bool) {}
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int, bool) const {}
void ImageBlock::prepareCache() const {}
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(FsFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile&) { return nullptr; }

void mathml::renderDrawing(const Drawing&, GfxRenderer& renderer, int, int x, int y, bool) {
  renderer.mathOrigins.emplace_back(x, y);
}

namespace {

mathml::Drawing exampleDrawing() {
  mathml::Drawing drawing;
  drawing.width = 24;
  drawing.ascent = 17;
  drawing.descent = 13;
  drawing.fontSize = 18;
  drawing.commandCount = 4;
  drawing.commands = std::make_unique<mathml::DrawCommand[]>(drawing.commandCount);
  drawing.commands[0] = {0, 17, 12, 12, 5, mathml::DrawType::Glyph, 0};
  drawing.commands[1] = {12, 10, 0, 0, 0, mathml::DrawType::Text, EpdFontFamily::ITALIC};
  drawing.commands[2] = {0, 17, 23, 17, 1, mathml::DrawType::Rule, 0};
  drawing.commands[3] = {0, 24, 10, 4, 0, mathml::DrawType::Fill, 1};
  drawing.textBytes = 4;
  drawing.text = std::make_unique<char[]>(drawing.textBytes);
  std::memcpy(drawing.text.get(), "abc", drawing.textBytes);
  return drawing;
}

std::shared_ptr<TextBlock> text(const char* word) {
  return std::make_shared<TextBlock>(std::vector<std::string>{word}, std::vector<int16_t>{0},
                                     std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR}, std::vector<uint8_t>{},
                                     std::vector<uint16_t>{}, std::vector<uint16_t>{}, std::vector<uint8_t>{},
                                     std::vector<bool>{});
}

TEST(PageMathCacheTest, PreservesStandaloneDrawingAndRendersItInTextPass) {
  Page source;
  source.elements.push_back(std::make_unique<PageMath>(exampleDrawing(), 3, 7));
  auto data = std::make_shared<HostFileData>();
  FsFile file(data);
  ASSERT_TRUE(source.serialize(file));
  ASSERT_TRUE(file.seek(0));
  const auto result = Page::deserialize(file);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->elements.size(), 1u);
  ASSERT_EQ(result->elements[0]->getTag(), TAG_PageMath);
  const auto& drawing = static_cast<const PageMath&>(*result->elements[0]).getDrawing();
  EXPECT_EQ(drawing.width, 24);
  EXPECT_EQ(drawing.ascent, 17);
  EXPECT_EQ(drawing.descent, 13);
  EXPECT_EQ(drawing.fontSize, 18);
  EXPECT_EQ(drawing.commandCount, 4);
  EXPECT_EQ(drawing.commands[0].value, 5);
  EXPECT_EQ(drawing.commands[1].style, EpdFontFamily::ITALIC);
  EXPECT_EQ(drawing.commands[2].x2, 23);
  EXPECT_EQ(drawing.commands[3].type, mathml::DrawType::Fill);
  EXPECT_STREQ(drawing.text.get(), "abc");

  GfxRenderer renderer;
  result->renderImages(renderer, 0, 4, 9);
  EXPECT_TRUE(renderer.mathOrigins.empty());
  result->renderText(renderer, 0, 4, 9);
  EXPECT_EQ(renderer.mathOrigins, (std::vector<std::pair<int, int>>{{7, 16}}));
}

TEST(PageMathCacheTest, RejectsTruncationAndInvalidProseOffsets) {
  auto data = std::make_shared<HostFileData>();
  FsFile file(data);
  PageMath drawing(exampleDrawing(), 0, 0);
  ASSERT_TRUE(drawing.serialize(file));

  constexpr size_t headerSize = 2 * sizeof(int16_t) + 5 * sizeof(uint16_t) + sizeof(uint8_t);
  constexpr size_t textCommandOffset = headerSize + sizeof(mathml::DrawCommand);
  auto invalid = std::make_shared<HostFileData>(*data);
  invalid->bytes[textCommandOffset + offsetof(mathml::DrawCommand, value)] = 1;
  FsFile invalidFile(invalid);
  EXPECT_FALSE(PageMath::deserialize(invalidFile));

  for (size_t size = data->bytes.size(); size > 0; --size) {
    data->bytes.resize(size - 1);
    ASSERT_TRUE(file.seek(0));
    EXPECT_FALSE(PageMath::deserialize(file)) << "after truncation to " << size - 1 << " bytes";
  }
}

TEST(PageMathCacheTest, PreservesTableDrawingAndVisitsProseInVisualOrder) {
  TableFragmentRow row;
  row.height = 44;
  row.cells.resize(2);
  auto& mathLine = row.cells[0].lines.emplace_back(text("left first"));
  const InlineObjectPlacement formula[] = {{7, 11, 30, 17, InlineObjectKind::Math}};
  ASSERT_TRUE(mathLine.setInlineMath(formula, 10, 7));
  mathLine.math->placements[0].drawing = std::make_unique<PageMath>(exampleDrawing(), 11, 0);
  row.cells[0].lines.emplace_back(text("left second"));
  row.cells[1].lines.emplace_back(text("right first"));
  row.cells[1].lines.emplace_back(text("right second"));
  std::vector<TableFragmentRow> rows;
  rows.push_back(std::move(row));

  Page page;
  page.elements.push_back(std::make_unique<PageTableFragment>(100, 2, 2, 10, std::move(rows), 3, 5));
  auto data = std::make_shared<HostFileData>();
  FsFile file(data);
  ASSERT_TRUE(page.serialize(file));
  ASSERT_TRUE(file.seek(0));
  const auto result = Page::deserialize(file);
  ASSERT_TRUE(result);

  using Visit = std::vector<std::pair<std::string, int>>;
  Visit visited;
  ASSERT_TRUE(result->forEachTextLine(
      [](const PageTextLine& line, void* context) {
        static_cast<Visit*>(context)->emplace_back(line.block->wordText(0), line.yPos);
        return true;
      },
      &visited));
  EXPECT_EQ(visited, (Visit{{"right first", 7}, {"left first", 17}, {"right second", 17}, {"left second", 37}}));

  GfxRenderer renderer;
  result->renderText(renderer, 0, 0, 0);
  EXPECT_EQ(renderer.mathOrigins, (std::vector<std::pair<int, int>>{{16, 7}}));
}

}  // namespace
