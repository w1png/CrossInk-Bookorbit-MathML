#include <Epub/math/MathCapture.h>
#include <GfxRenderer.h>
#include <expat.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using mathml::Drawing;
using mathml::DrawType;

class MathLayoutTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;

  void SetUp() override { renderer.textAdvancePerChar = 7; }

  bool layout(std::string_view source, Drawing& drawing, uint16_t width = 320, uint16_t height = 480,
              bool display = true) {
    auto capture = std::make_unique<mathml::Capture>();
    auto* parser = XML_ParserCreate(nullptr);
    if (!parser) return false;
    XML_SetUserData(parser, capture.get());
    XML_SetElementHandler(
        parser,
        [](void* data, const char* name, const char** attributes) {
          static_cast<mathml::Capture*>(data)->start(name, attributes);
        },
        [](void* data, const char*) { static_cast<mathml::Capture*>(data)->end(); });
    XML_SetCharacterDataHandler(parser, [](void* data, const char* text, int length) {
      static_cast<mathml::Capture*>(data)->characters({text, static_cast<size_t>(length)});
    });
    const auto status = XML_Parse(parser, source.data(), static_cast<int>(source.size()), XML_TRUE);
    XML_ParserFree(parser);
    return status == XML_STATUS_OK && capture->valid() &&
           mathml::layout(capture->nodes(), renderer, 0, width, height, 0, display, drawing);
  }
};

auto commands(const Drawing& drawing) { return std::span(drawing.commands.get(), drawing.commandCount); }

TEST_F(MathLayoutTest, PlacesBothScriptsAtTheSameSideOnDifferentBaselines) {
  Drawing drawing;
  ASSERT_TRUE(layout("<math><msubsup><mi>x</mi><mi>i</mi><mi>y</mi></msubsup></math>", drawing));
  ASSERT_EQ(drawing.commandCount, 3);
  const auto& base = drawing.commands[0];
  const auto& sub = drawing.commands[1];
  const auto& sup = drawing.commands[2];
  EXPECT_EQ(base.type, DrawType::Glyph);
  EXPECT_EQ(sub.type, DrawType::Glyph);
  EXPECT_EQ(sup.type, DrawType::Glyph);
  EXPECT_GT(sub.x, base.x);
  EXPECT_GT(sup.x, base.x);
  EXPECT_GT(sub.y, base.y);
  EXPECT_LT(sup.y, base.y);
  EXPECT_LT(sub.x2, base.x2);
  EXPECT_LT(sup.x2, base.x2);
}

TEST_F(MathLayoutTest, StacksFractionWithinAnIndexedRadical) {
  Drawing drawing;
  ASSERT_TRUE(layout("<math><mroot><mfrac><mi>a</mi><mi>b</mi></mfrac><mn>3</mn></mroot></math>", drawing));
  const auto result = commands(drawing);
  ASSERT_GE(result.size(), 6);
  EXPECT_GT(result[1].y, result[0].y);
  EXPECT_EQ(std::count_if(result.begin(), result.end(), [](const auto& item) { return item.type == DrawType::Rule; }),
            2);
  EXPECT_GT(drawing.descent, 0);
}

TEST_F(MathLayoutTest, AlignsMatrixCellsByRowAndColumn) {
  Drawing drawing;
  ASSERT_TRUE(
      layout("<math><mtable><mtr><mtd><mn>1</mn></mtd><mtd><mn>2</mn></mtd></mtr>"
             "<mtr><mtd><mn>3</mn></mtd><mtd><mn>4</mn></mtd></mtr></mtable></math>",
             drawing));
  ASSERT_EQ(drawing.commandCount, 4);
  const auto result = commands(drawing);
  EXPECT_EQ(result[0].y, result[1].y);
  EXPECT_EQ(result[2].y, result[3].y);
  EXPECT_LT(result[0].y, result[2].y);
  EXPECT_EQ(result[0].x, result[2].x);
  EXPECT_EQ(result[1].x, result[3].x);
  EXPECT_LT(result[0].x, result[1].x);
}

TEST_F(MathLayoutTest, KeepsDisplayFractionsWholeAcrossLineWraps) {
  Drawing drawing;
  ASSERT_TRUE(
      layout("<math><mrow><mfrac><mn>1</mn><mn>2</mn></mfrac><mo>+</mo>"
             "<mfrac><mn>3</mn><mn>4</mn></mfrac><mo>+</mo><mfrac><mn>5</mn><mn>6</mn></mfrac>"
             "</mrow></math>",
             drawing, 27));
  EXPECT_LE(drawing.width, 27);
  const auto result = commands(drawing);
  int rules = 0;
  int wraps = 0;
  int previousRule = -1;
  for (const auto& item : result) {
    if (item.type != DrawType::Rule) continue;
    EXPECT_GE(item.y, previousRule);
    if (rules > 0 && item.y > previousRule) ++wraps;
    ++rules;
    previousRule = item.y;
  }
  EXPECT_EQ(rules, 3);
  EXPECT_GT(wraps, 0);
}

TEST_F(MathLayoutTest, SelectsPresentationAndKeepsPhantomSpaceWithoutInk) {
  Drawing drawing;
  ASSERT_TRUE(
      layout("<math><mphantom><mi>W</mi></mphantom><semantics><mi>x</mi>"
             "<annotation encoding='application/x-tex'>ignored</annotation></semantics></math>",
             drawing));
  ASSERT_EQ(drawing.commandCount, 1);
  EXPECT_GT(drawing.commands[0].x, 0);
  EXPECT_EQ(drawing.textBytes, 0);
}

TEST_F(MathLayoutTest, RejectsDeepLayoutWithoutPublishingPartialCommands) {
  Drawing drawing;
  std::string source = "<math>";
  for (int i = 0; i < 14; ++i) source += "<mrow>";
  source += "<mi>x</mi>";
  for (int i = 0; i < 14; ++i) source += "</mrow>";
  source += "</math>";
  EXPECT_FALSE(layout(source, drawing));
  EXPECT_EQ(drawing.commandCount, 0);
  EXPECT_FALSE(drawing.commands);
}

}  // namespace
