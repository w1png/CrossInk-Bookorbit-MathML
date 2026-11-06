#include "MathLayout.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MathFont.h"
#include "OperatorDictionaryData.h"

namespace mathml {
namespace {

constexpr uint16_t MAX_NODES = 192;
constexpr int MAX_DEPTH = 11;
constexpr int MAX_LAYOUT_STACK_BYTES = 4608;
constexpr uint16_t MAX_LAYOUT_COMMANDS = 320;
constexpr uint16_t MAX_LAYOUT_TEXT = 1536;

using Constant = MathFont::Constant;
using Glyph = MathFont::Glyph;
using Direction = MathFont::Direction;

enum class Form : uint8_t { Infix, Prefix, Postfix };
enum OperatorFlag : uint8_t { Stretchy = 4, Symmetric = 8, Large = 16, MovableLimits = 32, Accent = 64 };

struct Box {
  int16_t width = 0;
  int16_t ascent = 0;
  int16_t descent = 0;
  uint16_t first = 0;
  uint16_t count = 0;
};
static_assert(sizeof(Box) == 10);

struct Style {
  int em;
  int scriptLevel = 0;
  bool display = false;
  bool rtl = false;
  std::string_view variant;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trim(std::string_view value) {
  while (!value.empty() && isSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && isSpace(value.back())) value.remove_suffix(1);
  return value;
}

std::optional<std::string_view> attribute(const Node& node, std::string_view name) {
  auto pairs = node.attributes;
  while (!pairs.empty()) {
    const auto keyEnd = pairs.find('\0');
    if (keyEnd == pairs.npos) break;
    const auto key = pairs.substr(0, keyEnd);
    pairs.remove_prefix(keyEnd + 1);
    const auto valueEnd = pairs.find('\0');
    if (valueEnd == pairs.npos) break;
    if (key == name) return trim(pairs.substr(0, valueEnd));
    pairs.remove_prefix(valueEnd + 1);
  }
  return {};
}

std::string_view attr(const Node& node, std::string_view name) {
  return attribute(node, name).value_or(std::string_view());
}

int roundedScale(int value, int numerator, int denominator) {
  const int magnitude = (std::abs(value) * numerator + denominator / 2) / denominator;
  return value < 0 ? -magnitude : magnitude;
}

std::optional<float> number(std::string_view& value) {
  if (value.empty()) return {};
  char buffer[24];
  const size_t copied = std::min(value.size(), sizeof(buffer) - 1);
  memcpy(buffer, value.data(), copied);
  buffer[copied] = '\0';
  char* end = nullptr;
  const float result = std::strtof(buffer, &end);
  if (end == buffer || !std::isfinite(result)) return {};
  value.remove_prefix(static_cast<size_t>(end - buffer));
  return result;
}

int length(std::string_view value, int em, int fallback, int percentageBase = 0, const Box* child = nullptr) {
  if (value.empty()) return fallback;
  const bool negative = value.starts_with("negative");
  auto named = negative ? value.substr(8) : value;
  if (named.ends_with("mathspace")) {
    named.remove_suffix(9);
    if (named == "medium") return roundedScale(negative ? -4 : 4, em, 18);
    int qualifiers = 0;
    while (named.starts_with("very")) {
      qualifiers++;
      named.remove_prefix(4);
    }
    const int units = named == "thin" ? 3 - qualifiers : named == "thick" ? 5 + qualifiers : 0;
    return units > 0 && units <= 7 ? roundedScale(negative ? -units : units, em, 18) : fallback;
  }
  const auto parsed = number(value);
  if (!parsed) return fallback;
  float multiplier = 1;
  if (value == "em")
    multiplier = em;
  else if (value == "ex")
    multiplier = em * 0.45f;
  else if (value == "pt")
    multiplier = 4.0f / 3;
  else if (value == "pc")
    multiplier = 16;
  else if (value == "in")
    multiplier = 96;
  else if (value == "cm")
    multiplier = 96.0f / 2.54f;
  else if (value == "mm")
    multiplier = 96.0f / 25.4f;
  else if (value == "%")
    multiplier = percentageBase / 100.0f;
  else if (child && value.ends_with("width"))
    multiplier = child->width / (value.starts_with('%') ? 100.0f : 1);
  else if (child && value.ends_with("height"))
    multiplier = child->ascent / (value.starts_with('%') ? 100.0f : 1);
  else if (child && value.ends_with("depth"))
    multiplier = child->descent / (value.starts_with('%') ? 100.0f : 1);
  else if (!value.empty() && value != "px")
    return fallback;
  return static_cast<int>(std::lround(std::clamp(*parsed * multiplier, -4095.0f, 4095.0f)));
}

int attrInt(const Node& node, std::string_view name, int fallback) {
  auto value = attr(node, name);
  const auto parsed = number(value);
  return parsed && value.empty() ? static_cast<int>(std::clamp(*parsed, -4095.0f, 4095.0f)) : fallback;
}

operatorDictionary::Properties operatorProperties(std::string_view text, Form form) {
  using namespace operatorDictionary;
  if (text.empty()) return {0, 0, 0};
  const auto* first = reinterpret_cast<const unsigned char*>(text.data());
  const auto* next = first;
  const uint32_t codepoint = utf8NextCodepoint(&next);
  for (const auto candidate : {form, Form::Infix, Form::Prefix, Form::Postfix}) {
    if (static_cast<size_t>(next - first) == text.size() && codepoint < 0x4000) {
      const auto key = static_cast<uint16_t>((codepoint << 2) | static_cast<uint8_t>(candidate));
      const auto* found = std::lower_bound(std::begin(SINGLE_KEYS), std::end(SINGLE_KEYS), key);
      if (found != std::end(SINGLE_KEYS) && *found == key) return PROPERTIES[SINGLE_PROPERTIES[found - SINGLE_KEYS]];
    } else if (text.size() <= 7) {
      const auto* found =
          std::lower_bound(std::begin(MULTI_RECORDS), std::end(MULTI_RECORDS), text,
                           [](const MultiRecord& entry, std::string_view token) {
                             return std::string_view(MULTI_TEXT + entry.textOffset, entry.textLength) < token;
                           });
      while (found != std::end(MULTI_RECORDS) &&
             std::string_view(MULTI_TEXT + found->textOffset, found->textLength) == text) {
        if (found->form == static_cast<uint8_t>(candidate)) return PROPERTIES[found->propertyIndex];
        ++found;
      }
    }
  }
  return {5, 5, 0};
}

Form explicitForm(const Node& node, Form fallback) {
  const auto value = attr(node, "form");
  if (value == "prefix") return Form::Prefix;
  if (value == "infix") return Form::Infix;
  if (value == "postfix") return Form::Postfix;
  return fallback;
}

uint8_t operatorFlags(const Node& node, std::string_view text, Form form) {
  uint8_t flags = operatorProperties(text, explicitForm(node, form)).flags;
  constexpr std::pair<std::string_view, uint8_t> OVERRIDES[] = {{"stretchy", Stretchy},
                                                                {"symmetric", Symmetric},
                                                                {"largeop", Large},
                                                                {"movablelimits", MovableLimits},
                                                                {"accent", Accent}};
  for (const auto& [name, flag] : OVERRIDES) {
    const auto value = attr(node, name);
    if (value == "true") flags |= flag;
    if (value == "false") flags &= ~flag;
  }
  return flags;
}

class Layout {
 public:
  Layout(std::span<const Node> nodes, GfxRenderer& renderer, int fontId, uint8_t fontSize, int width, int height,
         Drawing& drawing, Box* boxes, uint16_t commands, uint16_t text);
  bool run(bool display);

 private:
  Box measure(uint16_t index, Style style, int depth, Form form = Form::Infix, int stretch = 0,
              Direction direction = Direction::Vertical, bool wrap = false);
  [[gnu::noinline]] Box row(const Node& node, Style style, int depth, bool wrap);
  [[gnu::noinline]] Box token(const Node& node, std::string_view text, Style style, Form form, int stretch,
                              Direction direction);
  Box glyph(Glyph glyphId, int em, int target = 0, Direction direction = Direction::Vertical, bool symmetric = false);
  Box singleGlyph(Glyph glyphId, int xEm, int yEm);
  Box readerText(std::string_view text, int em, uint8_t style = 0);
  // Keep these out of the recursive dispatch frame. Foreground section builds
  // can run on the C3 loop task's 8 KiB stack.
  [[gnu::noinline]] Box fraction(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box radical(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box scripts(const Node& node, Style style, int depth, Form form);
  [[gnu::noinline]] Box multiscripts(const Node& node, Style style, int depth, Form form);
  [[gnu::noinline]] Box fenced(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box table(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box enclosure(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box stack(const Node& node, Style style, int depth);
  [[gnu::noinline]] Box mixedToken(const Node& node, Style style, int depth, Form form);
  [[gnu::noinline]] Box padded(const Node& node, Style style, int depth);
  Style styled(const Node& node, Style style);
  Style script(Style style, int levels = 1);
  int metric(Constant constant, int em) const;
  Box bounds(int width, int ascent, int descent, uint16_t first);
  void move(const Box& box, int x, int y);
  void add(DrawType type, int x, int y, int x2, int y2, uint16_t value, uint8_t style = 0);
  void rule(int x, int y, int x2, int y2, int thickness = 1);
  std::string_view tokenText(const Node& node) const;
  const Node* coreOperator(uint16_t node) const;

  std::span<const Node> nodes_;
  GfxRenderer& renderer_;
  int fontId_;
  int baseEm_;
  int readerAscent_;
  int readerDescent_;
  int width_;
  int height_;
  Drawing& drawing_;
  Box* boxes_;
  uint16_t commandCapacity_;
  uint16_t textCapacity_;
  int stackBytes_ = 0;
  bool failed_ = false;
  MathFont font_{bundledMathFontBytes()};
  MathFont::Construction construction_{};
};

Layout::Layout(std::span<const Node> nodes, GfxRenderer& renderer, int fontId, uint8_t fontSize, int width, int height,
               Drawing& drawing, Box* boxes, uint16_t commands, uint16_t text)
    : nodes_(nodes),
      renderer_(renderer),
      fontId_(renderer.getFontIdForSize(fontId, fontSize)),
      baseEm_(std::clamp(renderer.getLineHeight(fontId_) * 4 / 5, 8, 72)),
      readerAscent_(renderer.getFontAscenderSize(fontId_)),
      readerDescent_(std::max(0, renderer.getLineHeight(fontId_) - readerAscent_)),
      width_(width),
      height_(height),
      drawing_(drawing),
      boxes_(boxes),
      commandCapacity_(commands),
      textCapacity_(text) {}

int Layout::metric(Constant constant, int em) const {
  return roundedScale(font_.constant(constant), em, font_.unitsPerEm());
}

Box Layout::bounds(int width, int ascent, int descent, uint16_t first) {
  if (width < -Drawing::MAX_DIMENSION || width > Drawing::MAX_DIMENSION || ascent > Drawing::MAX_DIMENSION ||
      descent > Drawing::MAX_DIMENSION)
    failed_ = true;
  return {static_cast<int16_t>(std::clamp<int>(width, -Drawing::MAX_DIMENSION, Drawing::MAX_DIMENSION)),
          static_cast<int16_t>(std::clamp<int>(ascent, 0, Drawing::MAX_DIMENSION)),
          static_cast<int16_t>(std::clamp<int>(descent, 0, Drawing::MAX_DIMENSION)), first,
          static_cast<uint16_t>(drawing_.commandCount - first)};
}

void Layout::move(const Box& box, int x, int y) {
  if ((!x && !y) || failed_) return;
  for (int i = box.first; i < box.first + box.count; ++i) {
    auto& cmd = drawing_.commands[i];
    const auto shift = [&](int16_t& coordinate, int amount) {
      const int shifted = coordinate + amount;
      if (std::abs(shifted) > Drawing::MAX_DIMENSION) {
        failed_ = true;
      } else {
        coordinate = static_cast<int16_t>(shifted);
      }
    };
    shift(cmd.x, x);
    shift(cmd.y, y);
    if (cmd.type == DrawType::Rule) {
      shift(cmd.x2, x);
      shift(cmd.y2, y);
    }
  }
}

void Layout::add(DrawType type, int x, int y, int x2, int y2, uint16_t value, uint8_t style) {
  if (drawing_.commandCount >= commandCapacity_ || std::abs(x) > Drawing::MAX_DIMENSION ||
      std::abs(y) > Drawing::MAX_DIMENSION || std::abs(x2) > Drawing::MAX_DIMENSION ||
      std::abs(y2) > Drawing::MAX_DIMENSION) {
    failed_ = true;
    return;
  }
  drawing_.commands[drawing_.commandCount++] = {static_cast<int16_t>(x),
                                                static_cast<int16_t>(y),
                                                static_cast<int16_t>(x2),
                                                static_cast<int16_t>(y2),
                                                value,
                                                type,
                                                style};
}

void Layout::rule(int x, int y, int x2, int y2, int thickness) {
  add(DrawType::Rule, x, y, x2, y2, std::clamp(thickness, 1, 16));
}

std::string_view Layout::tokenText(const Node& node) const {
  if (!node.text.empty()) return trim(node.text);
  if (node.firstChild < nodes_.size() && nodes_[node.firstChild].name == "#text" &&
      nodes_[node.firstChild].nextSibling == NO_NODE)
    return trim(nodes_[node.firstChild].text);
  return {};
}

const Node* Layout::coreOperator(uint16_t index) const {
  for (int depth = 0; index < nodes_.size() && depth < MAX_DEPTH; ++depth) {
    const auto& node = nodes_[index];
    if (node.name == "mo") return &node;
    if (node.name != "munder" && node.name != "mover" && node.name != "munderover" && node.name != "msub" &&
        node.name != "msup" && node.name != "msubsup" && node.name != "mmultiscripts")
      return nullptr;
    index = node.firstChild;
  }
  return nullptr;
}

Style Layout::script(Style style, int levels) {
  const int scriptPercent = std::max(1, static_cast<int>(font_.constant(Constant::ScriptPercentScaleDown)));
  for (int i = 0; i < std::abs(levels); ++i) {
    style.em = levels > 0 ? std::max(6, roundedScale(style.em, scriptPercent, 100))
                          : std::min(96, roundedScale(style.em, 100, scriptPercent));
  }
  style.scriptLevel += levels;
  style.display = false;
  return style;
}

Style Layout::styled(const Node& node, Style style) {
  const auto display = attr(node, "displaystyle");
  const auto level = attr(node, "scriptlevel");
  if (!level.empty()) {
    const int value = attrInt(node, "scriptlevel", style.scriptLevel);
    const int next = level.starts_with('+') || level.starts_with('-') ? style.scriptLevel + value : value;
    style = script(style, std::clamp(next - style.scriptLevel, -6, 6));
  }
  if (display == "true" || display == "false") style.display = display == "true";
  const auto dir = attr(node, "dir");
  if (dir == "rtl" || dir == "ltr") style.rtl = dir == "rtl";
  const auto variant = attr(node, "mathvariant");
  if (!variant.empty()) style.variant = variant;
  const auto size = attr(node, "mathsize");
  if (size == "small")
    style.em = std::max(6, style.em * 4 / 5);
  else if (size == "big")
    style.em = std::min(96, style.em * 5 / 4);
  else
    style.em = std::clamp(length(size, style.em, style.em, style.em), 6, 96);
  return style;
}

Box Layout::readerText(std::string_view text, int em, uint8_t style) {
  const auto first = drawing_.commandCount;
  if (text.empty()) return bounds(0, 0, 0, first);
  if (text.size() + 1 > static_cast<size_t>(textCapacity_ - drawing_.textBytes)) {
    failed_ = true;
    return bounds(0, 0, 0, first);
  }
  const bool small = em * 4 < baseEm_ * 3;
  if (small) style |= EpdFontFamily::SUP;
  const auto start = drawing_.textBytes;
  char* output = drawing_.text.get() + start;
  bool previousWasSpace = false;
  for (char c : text) {
    const bool space = isSpace(c);
    if (!space || !previousWasSpace) {
      output[drawing_.textBytes++ - start] = space ? ' ' : c;
    }
    previousWasSpace = space;
  }
  output[drawing_.textBytes++ - start] = '\0';
  const int advance = renderer_.getTextAdvanceX(fontId_, output, static_cast<EpdFontFamily::Style>(style));
  add(DrawType::Text, 0, -readerAscent_, 0, 0, start, style);
  return bounds(advance, small ? (readerAscent_ + 1) / 2 : readerAscent_,
                small ? (readerDescent_ + 1) / 2 : readerDescent_, first);
}

Box Layout::singleGlyph(Glyph glyphId, int xEm, int yEm) {
  const auto first = drawing_.commandCount;
  const auto info = font_.metrics(glyphId);
  if (!info) {
    failed_ = true;
    return {};
  }
  const int advance = roundedScale(info->advance, xEm, font_.unitsPerEm());
  if (!info->width || !info->height) return bounds(advance, 0, 0, first);
  const int left = roundedScale(info->left, xEm, font_.strikePpem());
  const int top = roundedScale(info->top, yEm, font_.strikePpem());
  const int width = std::max(1, roundedScale(info->width, xEm, font_.strikePpem()));
  const int height = std::max(1, roundedScale(info->height, yEm, font_.strikePpem()));
  const int inset = std::max(0, -left);
  add(DrawType::Glyph, inset, 0, xEm, yEm, glyphId);
  return bounds(std::max(advance, left + width) + inset, top, height - top, first);
}

Box Layout::glyph(Glyph glyphId, int em, int target, Direction direction, bool symmetric) {
  const auto first = drawing_.commandCount;
  const bool horizontal = direction == Direction::Horizontal;
  if (target > 0 && font_.construction(glyphId, direction, construction_)) {
    const int unitsPerEm = font_.unitsPerEm();
    for (int i = 0; i < construction_.variantCount; ++i) {
      const auto& variant = construction_.variants[i];
      glyphId = variant.glyph;
      if (roundedScale(variant.advance, em, unitsPerEm) >= target) break;
    }
    const bool haveVariant =
        construction_.variantCount &&
        roundedScale(construction_.variants[construction_.variantCount - 1].advance, em, unitsPerEm) >= target;
    if (!haveVariant && construction_.partCount) {
      // OpenType parts are ordered bottom-to-top for vertical assemblies and
      // left-to-right for horizontal assemblies. Repeat all extender parts at
      // each step so a multi-extender bracket stays centered.
      int repeats = 0;
      int total = 0;
      const int overlap = std::max(0, static_cast<int>(font_.constant(Constant::MinConnectorOverlap)));
      for (; repeats <= 20; ++repeats) {
        total = 0;
        int previousConnector = 0;
        for (int i = 0; i < construction_.partCount; ++i) {
          const auto& part = construction_.parts[i];
          for (int copy = 0; copy < (part.extender ? repeats : 1); ++copy) {
            total += part.advance - std::min({overlap, previousConnector, static_cast<int>(part.startConnector)});
            previousConnector = part.endConnector;
          }
        }
        if (roundedScale(total, em, unitsPerEm) >= target) break;
      }
      if (repeats <= 20 && total > 0) {
        int cursor = 0;
        int previousConnector = 0;
        int ascent = 0;
        int descent = 0;
        int width = 0;
        const int extent = roundedScale(total, em, unitsPerEm);
        const int bottom = extent / 2 - metric(Constant::AxisHeight, em);
        for (int i = 0; i < construction_.partCount; ++i) {
          const auto& part = construction_.parts[i];
          for (int copy = 0; copy < (part.extender ? repeats : 1); ++copy) {
            cursor -= std::min({overlap, previousConnector, static_cast<int>(part.startConnector)});
            const int position = roundedScale(cursor, em, unitsPerEm);
            auto item = singleGlyph(part.glyph, em, em);
            if (horizontal) {
              move(item, position, 0);
              width = std::max(width, position + item.width);
              ascent = std::max<int>(ascent, item.ascent);
              descent = std::max<int>(descent, item.descent);
            } else {
              const int y = bottom - position - item.descent;
              move(item, 0, y);
              width = std::max<int>(width, item.width);
              ascent = std::max(ascent, item.ascent - y);
              descent = std::max(descent, item.descent + y);
            }
            previousConnector = part.endConnector;
            cursor += part.advance;
          }
        }
        return bounds(width, ascent, descent, first);
      }
    }
  }
  auto result = singleGlyph(glyphId, em, em);
  if (symmetric && target > 0 && !horizontal) {
    const int y = (result.ascent - result.descent) / 2 - metric(Constant::AxisHeight, em);
    move(result, 0, y);
    result = bounds(result.width, result.ascent - y, result.descent + y, first);
  }
  return result;
}

[[gnu::noinline]] uint32_t variantCharacter(uint32_t c, std::string_view variant) {
  struct Alphabet {
    std::string_view name;
    uint32_t capitals;
    uint32_t digits;
  };
  static constexpr Alphabet ALPHABETS[] = {
      {"bold", 0x1D400, 0x1D7CE},          {"italic", 0x1D434, 0},
      {"bold-italic", 0x1D468, 0},         {"script", 0x1D49C, 0},
      {"bold-script", 0x1D4D0, 0},         {"fraktur", 0x1D504, 0},
      {"double-struck", 0x1D538, 0x1D7D8}, {"bold-fraktur", 0x1D56C, 0},
      {"sans-serif", 0x1D5A0, 0x1D7E2},    {"bold-sans-serif", 0x1D5D4, 0x1D7EC},
      {"sans-serif-italic", 0x1D608, 0},   {"sans-serif-bold-italic", 0x1D63C, 0},
      {"monospace", 0x1D670, 0x1D7F6},
  };
  for (const auto& alphabet : ALPHABETS) {
    if (alphabet.name != variant) continue;
    if (c >= 'A' && c <= 'Z')
      c = alphabet.capitals + c - 'A';
    else if (c >= 'a' && c <= 'z')
      c = alphabet.capitals + 26 + c - 'a';
    else if (c >= '0' && c <= '9' && alphabet.digits)
      c = alphabet.digits + c - '0';
    break;
  }
  constexpr std::pair<uint32_t, uint16_t> LETTERLIKE[] = {
      {0x1D455, 0x210E}, {0x1D49D, 0x212C}, {0x1D4A0, 0x2130}, {0x1D4A1, 0x2131}, {0x1D4A3, 0x210B}, {0x1D4A4, 0x2110},
      {0x1D4A7, 0x2112}, {0x1D4A8, 0x2133}, {0x1D4AD, 0x211B}, {0x1D4BA, 0x212F}, {0x1D4BC, 0x210A}, {0x1D4C4, 0x2134},
      {0x1D506, 0x212D}, {0x1D50B, 0x210C}, {0x1D50C, 0x2111}, {0x1D515, 0x211C}, {0x1D51D, 0x2128}, {0x1D53A, 0x2102},
      {0x1D53F, 0x210D}, {0x1D545, 0x2115}, {0x1D547, 0x2119}, {0x1D548, 0x211A}, {0x1D549, 0x211D}, {0x1D551, 0x2124},
  };
  for (const auto& [from, to] : LETTERLIKE) {
    if (c == from) return to;
  }
  const uint32_t greek = variant == "bold"                     ? 0x1D6A8
                         : variant == "italic"                 ? 0x1D6E2
                         : variant == "bold-italic"            ? 0x1D71C
                         : variant == "bold-sans-serif"        ? 0x1D756
                         : variant == "sans-serif-bold-italic" ? 0x1D790
                                                               : 0;
  if (greek) {
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return greek + c - 0x391;
    if (c >= 0x3B1 && c <= 0x3C9) return greek + 26 + c - 0x3B1;
  }
  return c;
}

Box Layout::token(const Node& node, std::string_view text, Style style, Form form, int stretch, Direction direction) {
  const auto first = drawing_.commandCount;
  if (text.empty()) return bounds(0, 0, 0, first);
  if (node.name == "mtext" || node.name == "mglyph") return readerText(text, style.em);

  auto variant = style.variant;
  auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* afterFirst = ptr;
  const auto* end = ptr + text.size();
  utf8NextCodepoint(&afterFirst);
  const bool single = afterFirst == end;
  if (variant.empty() && node.name == "mi" && single) variant = "italic";

  int x = 0;
  int right = 0;
  int ascent = 0;
  int descent = 0;
  uint8_t flags = 0;
  if (node.name == "mo") {
    form = explicitForm(node, form);
    const auto properties = operatorProperties(text, form);
    flags = operatorFlags(node, text, form);
    x = length(attr(node, "lspace"), style.em, roundedScale(properties.lspace, style.em, 18));
    right = length(attr(node, "rspace"), style.em, roundedScale(properties.rspace, style.em, 18));
    if (!(flags & Stretchy)) stretch = 0;
    if ((flags & Large) && style.display && direction == Direction::Vertical) {
      stretch = std::max(stretch, metric(Constant::DisplayOperatorMinHeight, style.em));
    }
    const int target = stretch;
    if (target > 0) {
      stretch = std::max(stretch, length(attr(node, "minsize"), style.em, 0, target));
      stretch = std::min(stretch, length(attr(node, "maxsize"), style.em, Drawing::MAX_DIMENSION, target));
    }
  }

  while (ptr < end && !failed_) {
    const auto* start = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);
    const uint32_t styledCp = variantCharacter(cp, variant);
    auto glyphId = font_.glyphForCodepoint(styledCp);
    if (glyphId == MathFont::MissingGlyph && styledCp != cp) glyphId = font_.glyphForCodepoint(cp);
    Box item;
    if (glyphId == MathFont::MissingGlyph) {
      uint8_t fallbackStyle = EpdFontFamily::REGULAR;
      if (variant.find("italic") != variant.npos) fallbackStyle |= EpdFontFamily::ITALIC;
      if (variant.find("bold") != variant.npos) fallbackStyle |= EpdFontFamily::BOLD;
      item =
          readerText({reinterpret_cast<const char*>(start), static_cast<size_t>(ptr - start)}, style.em, fallbackStyle);
    } else {
      item = glyph(glyphId, style.em, single ? stretch : 0, direction, (flags & Symmetric) != 0);
    }
    move(item, x, 0);
    x += item.width;
    ascent = std::max<int>(ascent, item.ascent);
    descent = std::max<int>(descent, item.descent);
  }
  return bounds(x + right, ascent, descent, first);
}

Box Layout::row(const Node& node, Style style, int depth, bool wrap) {
  const auto first = drawing_.commandCount;
  const auto formFor = [&](uint16_t child) {
    if (child == node.firstChild && nodes_[child].nextSibling != NO_NODE) return Form::Prefix;
    return nodes_[child].nextSibling == NO_NODE && child != node.firstChild ? Form::Postfix : Form::Infix;
  };
  int stretch = 0;
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    const auto& item = nodes_[child];
    const Form form = formFor(child);
    if (item.name == "mo" && (operatorFlags(item, tokenText(item), form) & Stretchy)) {
      boxes_[child].first = UINT16_MAX;
      continue;
    }
    const auto box = measure(child, style, depth, form);
    const int axis = metric(Constant::AxisHeight, style.em);
    stretch = std::max(stretch, 2 * std::max(box.ascent - axis, box.descent + axis));
  }
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    if (boxes_[child].first == UINT16_MAX) measure(child, style, depth, formFor(child), stretch);
  }

  int width = 0;
  int ascent = 0;
  int bottom = 0;
  bool firstLine = true;
  for (auto line = node.firstChild; line != NO_NODE && !failed_;) {
    auto end = line;
    int lineWidth = 0;
    int lineAscent = 0;
    int lineDescent = 0;
    do {
      const auto& box = boxes_[end];
      const auto& item = nodes_[end];
      const bool newline = wrap && attr(item, "linebreak") == "newline";
      const bool after = newline && attr(item, "linebreakstyle") == "after";
      if (wrap && end != line && (lineWidth + box.width > width_ || (newline && !after))) break;
      lineWidth += box.width;
      lineAscent = std::max<int>(lineAscent, box.ascent);
      lineDescent = std::max<int>(lineDescent, box.descent);
      end = item.nextSibling;
      if (after) break;
    } while (end != NO_NODE);

    const int baseline = firstLine ? 0 : bottom + lineAscent + std::max(2, style.em / 5);
    int x = style.rtl ? lineWidth : 0;
    for (auto child = line; child != end; child = nodes_[child].nextSibling) {
      const auto& box = boxes_[child];
      if (style.rtl) x -= box.width;
      move(box, x, baseline);
      if (!style.rtl) x += box.width;
    }
    if (firstLine) ascent = lineAscent;
    width = std::max(width, lineWidth);
    bottom = baseline + lineDescent;
    firstLine = false;
    line = end;
  }
  return bounds(width, ascent, bottom, first);
}

Box Layout::fraction(const Node& node, Style style, int depth) {
  const auto first = drawing_.commandCount;
  const auto numerator = node.firstChild;
  if (numerator == NO_NODE) return {};
  const auto denominator = nodes_[numerator].nextSibling;
  if (denominator == NO_NODE || nodes_[denominator].nextSibling != NO_NODE) return row(node, style, depth, false);

  Style fractionStyle = style.display ? style : script(style);
  fractionStyle.display = false;
  const auto top = measure(numerator, fractionStyle, depth);
  const auto bottom = measure(denominator, fractionStyle, depth);
  const int pad = std::max(1, style.em / 12);
  if (attr(node, "bevelled") == "true") {
    const int rise = std::max(2, metric(Constant::SkewedFractionVerticalGap, style.em) / 2);
    const int gap = std::max(2, metric(Constant::SkewedFractionHorizontalGap, style.em) / 2);
    const int slashWidth = std::max(3, style.em / 5);
    const int barLeft = top.width + gap;
    move(top, 0, -rise);
    move(bottom, barLeft + slashWidth + gap, rise);
    const int ascent = std::max<int>(top.ascent + rise, bottom.ascent - rise);
    const int descent = std::max<int>(top.descent - rise, bottom.descent + rise);
    rule(barLeft, descent, barLeft + slashWidth, -ascent);
    return bounds(barLeft + slashWidth + gap + bottom.width, ascent, descent, first);
  }

  const int axis = metric(Constant::AxisHeight, style.em);
  int thickness = std::max(1, metric(Constant::FractionRuleThickness, style.em));
  const auto line = attr(node, "linethickness");
  if (line == "thin")
    thickness = std::max(1, thickness / 2);
  else if (line == "thick")
    thickness *= 2;
  else
    thickness = std::clamp(length(line, style.em, thickness, thickness), 0, 16);
  int up;
  int down;
  if (thickness) {
    const auto numeratorShift =
        style.display ? Constant::FractionNumeratorDisplayStyleShiftUp : Constant::FractionNumeratorShiftUp;
    const auto numeratorGap =
        style.display ? Constant::FractionNumDisplayStyleGapMin : Constant::FractionNumeratorGapMin;
    const auto denominatorShift =
        style.display ? Constant::FractionDenominatorDisplayStyleShiftDown : Constant::FractionDenominatorShiftDown;
    const auto denominatorGap =
        style.display ? Constant::FractionDenomDisplayStyleGapMin : Constant::FractionDenominatorGapMin;
    up = std::max(metric(numeratorShift, style.em),
                  axis + thickness / 2 + std::max(1, metric(numeratorGap, style.em)) + top.descent);
    down = std::max(metric(denominatorShift, style.em),
                    -axis + (thickness + 1) / 2 + std::max(1, metric(denominatorGap, style.em)) + bottom.ascent);
  } else {
    up = metric(style.display ? Constant::StackTopDisplayStyleShiftUp : Constant::StackTopShiftUp, style.em);
    down =
        metric(style.display ? Constant::StackBottomDisplayStyleShiftDown : Constant::StackBottomShiftDown, style.em);
    const int gap = metric(style.display ? Constant::StackDisplayStyleGapMin : Constant::StackGapMin, style.em);
    const int extra = std::max(0, gap - (up + down - top.descent - bottom.ascent));
    up += extra / 2;
    down += (extra + 1) / 2;
  }
  const int width = std::max(top.width, bottom.width) + 2 * pad;
  const auto align = [&](std::string_view key, int childWidth) {
    const auto value = attr(node, key);
    if (value == "left") return pad;
    if (value == "right") return width - pad - childWidth;
    return (width - childWidth) / 2;
  };
  move(top, align("numalign", top.width), -up);
  move(bottom, align("denomalign", bottom.width), down);
  if (thickness) rule(0, -axis, width - 1, -axis, thickness);
  return bounds(width, top.ascent + up, bottom.descent + down, first);
}

Box Layout::radical(const Node& node, Style style, int depth) {
  const auto first = drawing_.commandCount;
  const auto radicand = node.firstChild;
  if (radicand == NO_NODE) return {};
  const bool indexed = node.name == "mroot";
  const auto degreeIndex = nodes_[radicand].nextSibling;
  if (indexed && (degreeIndex == NO_NODE || nodes_[degreeIndex].nextSibling != NO_NODE))
    return row(node, style, depth, false);
  const auto body = indexed ? measure(radicand, style, depth) : row(node, style, depth, false);
  const auto degree = indexed ? measure(degreeIndex, script(style, 2), depth) : Box{};
  const int gap = std::max(
      1, metric(style.display ? Constant::RadicalDisplayStyleVerticalGap : Constant::RadicalVerticalGap, style.em));
  const int thickness = std::max(1, metric(Constant::RadicalRuleThickness, style.em));
  const int target = body.ascent + body.descent + gap + thickness;
  const auto radical = glyph(font_.glyphForCodepoint(0x221A), style.em, target);
  const int signY = body.descent - radical.descent;
  const int degreeX = indexed ? std::max(0, metric(Constant::RadicalKernBeforeDegree, style.em)) : 0;
  const int signX =
      indexed ? std::max(0, degreeX + degree.width + metric(Constant::RadicalKernAfterDegree, style.em)) : 0;
  const int bodyX = signX + radical.width;
  const int bar = -body.ascent - gap;
  const int degreeY =
      body.descent -
      roundedScale(radical.ascent + radical.descent, font_.constant(Constant::RadicalDegreeBottomRaisePercent), 100) -
      degree.descent;
  move(radical, signX, signY);
  move(body, bodyX, 0);
  move(degree, degreeX, degreeY);
  if (body.width > 0) rule(std::max(0, bodyX - 1), bar, bodyX + body.width - 1, bar, thickness);
  return bounds(bodyX + body.width,
                std::max({-bar + std::max(1, metric(Constant::RadicalExtraAscender, style.em)), radical.ascent - signY,
                          degree.ascent - degreeY}),
                body.descent, first);
}

Box Layout::scripts(const Node& node, Style style, int depth, Form form) {
  const auto first = drawing_.commandCount;
  const auto baseIndex = node.firstChild;
  if (baseIndex == NO_NODE) return {};
  const auto one = nodes_[baseIndex].nextSibling;
  if (one == NO_NODE) return row(node, style, depth, false);
  const auto two = nodes_[one].nextSibling;
  const bool both = node.name == "msubsup" || node.name == "munderover";
  if (both ? (two == NO_NODE || nodes_[two].nextSibling != NO_NODE) : two != NO_NODE)
    return row(node, style, depth, false);
  const bool overOnly = node.name == "msup" || node.name == "mover";
  const auto below = overOnly ? NO_NODE : one;
  const auto above = overOnly ? one : two;
  bool stacked = node.name == "munder" || node.name == "mover" || node.name == "munderover";
  const auto* op = coreOperator(baseIndex);
  if (stacked && !style.display && op && (operatorFlags(*op, tokenText(*op), form) & MovableLimits)) stacked = false;
  const auto base = measure(baseIndex, style, depth, form);
  const auto accent = [&](uint16_t index, std::string_view name) {
    if (!stacked || index == NO_NODE) return false;
    const auto explicitValue = attr(node, name);
    if (explicitValue == "true" || explicitValue == "false") return explicitValue == "true";
    const auto* accentOp = coreOperator(index);
    return accentOp && (operatorFlags(*accentOp, tokenText(*accentOp), Form::Infix) & Accent);
  };
  const bool upperAccent = accent(above, "accent");
  const bool lowerAccent = accent(below, "accentunder");
  const auto sub = below == NO_NODE ? Box{}
                                    : measure(below, lowerAccent ? style : script(style), depth, Form::Infix,
                                              stacked ? base.width : 0, Direction::Horizontal);
  const auto sup = above == NO_NODE ? Box{}
                                    : measure(above, upperAccent ? style : script(style), depth, Form::Infix,
                                              stacked ? base.width : 0, Direction::Horizontal);

  // STIX and other math fonts expose italic correction and accent attachment
  // for single glyphs. A grouped box instead uses its geometric center.
  const auto* baseGlyph = base.count == 1 && drawing_.commands[base.first].type == DrawType::Glyph
                              ? &drawing_.commands[base.first]
                              : nullptr;
  const int italic =
      baseGlyph ? roundedScale(font_.italicCorrection(baseGlyph->value), style.em, font_.unitsPerEm()) : 0;
  if (stacked) {
    const int width = std::max({base.width, sub.width, sup.width});
    int baseX = (width - base.width) / 2;
    int upperX = (width - sup.width + italic) / 2;
    int lowerX = (width - sub.width - italic) / 2;
    const auto attachment = [&](const Box& box, int fallback) {
      if (box.count != 1) return fallback;
      const auto& cmd = drawing_.commands[box.first];
      if (cmd.type != DrawType::Glyph) return fallback;
      const auto value = font_.topAccent(cmd.value);
      return value ? cmd.x + roundedScale(*value, cmd.x2, font_.unitsPerEm()) : fallback;
    };
    if (upperAccent) upperX = baseX + attachment(base, base.width / 2) - attachment(sup, sup.width / 2);
    if (lowerAccent) lowerX = baseX + (base.width - sub.width) / 2;
    const int left = std::min({baseX, upperX, lowerX});
    baseX -= left;
    upperX -= left;
    lowerX -= left;
    const int up =
        upperAccent ? base.ascent + sup.descent + 1
                    : std::max(metric(Constant::UpperLimitBaselineRiseMin, style.em),
                               base.ascent + sup.descent + std::max(1, metric(Constant::UpperLimitGapMin, style.em)));
    const int down =
        lowerAccent ? base.descent + sub.ascent + 1
                    : std::max(metric(Constant::LowerLimitBaselineDropMin, style.em),
                               base.descent + sub.ascent + std::max(1, metric(Constant::LowerLimitGapMin, style.em)));
    move(base, baseX, 0);
    move(sub, lowerX, down);
    move(sup, upperX, -up);
    return bounds(std::max({baseX + base.width, lowerX + sub.width, upperX + sup.width}),
                  above == NO_NODE ? base.ascent : std::max<int>(base.ascent, up + sup.ascent),
                  below == NO_NODE ? base.descent : std::max<int>(base.descent, down + sub.descent), first);
  }

  int up = std::max({metric(Constant::SuperscriptShiftUp, style.em),
                     sup.descent + metric(Constant::SuperscriptBottomMin, style.em),
                     base.ascent - metric(Constant::SuperscriptBaselineDropMax, style.em)});
  int down = std::max({metric(Constant::SubscriptShiftDown, style.em),
                       sub.ascent - metric(Constant::SubscriptTopMax, style.em),
                       base.descent + metric(Constant::SubscriptBaselineDropMin, style.em)});
  if (both)
    down += std::max(0, metric(Constant::SubSuperscriptGapMin, style.em) - (up + down - sup.descent - sub.ascent));
  const auto kern = [&](const Box& attachment, int height, MathFont::Corner corner) {
    if (!baseGlyph || !attachment.count || height < 0) return 0;
    return roundedScale(font_.mathKern(baseGlyph->value, corner, roundedScale(height, font_.unitsPerEm(), style.em)),
                        style.em, font_.unitsPerEm());
  };
  const int subX = std::max(0, base.width + kern(sub, sub.ascent - down, MathFont::Corner::BottomRight));
  const int supX = std::max(0, base.width + italic + kern(sup, up - sup.descent, MathFont::Corner::TopRight));
  const int width = std::max({static_cast<int>(base.width), below == NO_NODE ? 0 : subX + sub.width,
                              above == NO_NODE ? 0 : supX + sup.width}) +
                    metric(Constant::SpaceAfterScript, style.em);
  if (style.rtl) {
    move(base, width - base.width, 0);
    move(sub, width - subX - sub.width, down);
    move(sup, width - supX - sup.width, -up);
  } else {
    move(sub, subX, down);
    move(sup, supX, -up);
  }
  return bounds(width, above == NO_NODE ? base.ascent : std::max<int>(base.ascent, sup.ascent + up),
                below == NO_NODE ? base.descent : std::max<int>(base.descent, sub.descent + down), first);
}

Box Layout::multiscripts(const Node& node, Style style, int depth, Form form) {
  const auto first = drawing_.commandCount;
  if (node.firstChild == NO_NODE) return {};
  const auto base = measure(node.firstChild, style, depth, form);
  auto child = nodes_[node.firstChild].nextSibling;
  bool prescript = false;
  int left = 0;
  int right = base.width;
  int ascent = base.ascent;
  int descent = base.descent;
  while (child != NO_NODE && !failed_) {
    if (nodes_[child].name == "mprescripts") {
      prescript = true;
      child = nodes_[child].nextSibling;
      continue;
    }
    const auto sub = measure(child, script(style), depth);
    child = nodes_[child].nextSibling;
    if (child == NO_NODE || nodes_[child].name == "mprescripts") {
      failed_ = true;
      break;
    }
    const auto sup = measure(child, script(style), depth);
    child = nodes_[child].nextSibling;
    const int up = std::max(metric(Constant::SuperscriptShiftUp, style.em),
                            sup.descent + metric(Constant::SuperscriptBottomMin, style.em));
    const int down = std::max({metric(Constant::SubscriptShiftDown, style.em),
                               sub.ascent - metric(Constant::SubscriptTopMax, style.em),
                               sup.descent + sub.ascent + metric(Constant::SubSuperscriptGapMin, style.em) - up});
    const int width = std::max(sub.width, sup.width) + metric(Constant::SpaceAfterScript, style.em);
    if (prescript) {
      move(sub, left - sub.width, down);
      move(sup, left - sup.width, -up);
      left -= width;
    } else {
      move(sub, right, down);
      move(sup, right, -up);
      right += width;
    }
    if (sup.count) ascent = std::max(ascent, sup.ascent + up);
    if (sub.count) descent = std::max(descent, sub.descent + down);
  }
  auto result = bounds(right - left, ascent, descent, first);
  move(result, -left, 0);
  return result;
}

Box Layout::fenced(const Node& node, Style style, int depth) {
  const auto first = drawing_.commandCount;
  const auto delimiter = [&](std::string_view value, Form form, int stretch) {
    Node operatorNode;
    operatorNode.name = "mo";
    return token(operatorNode, value, style, form, stretch, Direction::Vertical);
  };
  // An explicitly empty delimiter or separator disables it.
  const auto open = attribute(node, "open").value_or("(");
  const auto close = attribute(node, "close").value_or(")");
  const auto separators = attribute(node, "separators").value_or(",");
  int ascent = 0;
  int descent = 0;
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    const auto box = measure(child, style, depth);
    ascent = std::max<int>(ascent, box.ascent);
    descent = std::max<int>(descent, box.descent);
  }
  const int axis = metric(Constant::AxisHeight, style.em);
  const int stretch = 2 * std::max(ascent - axis, descent + axis);
  int x = 0;
  const auto place = [&](const Box& box) {
    move(box, x, 0);
    x += box.width;
    ascent = std::max<int>(ascent, box.ascent);
    descent = std::max<int>(descent, box.descent);
  };
  place(delimiter(open, Form::Prefix, stretch));
  auto remaining = separators;
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    place(boxes_[child]);
    if (nodes_[child].nextSibling == NO_NODE || remaining.empty()) continue;
    auto* start = reinterpret_cast<const unsigned char*>(remaining.data());
    const auto* next = start;
    utf8NextCodepoint(&next);
    const size_t bytes = next - start;
    place(delimiter(remaining.substr(0, bytes), Form::Infix, 0));
    if (bytes < remaining.size()) remaining = trim(remaining.substr(bytes));
  }
  place(delimiter(close, Form::Postfix, stretch));
  return bounds(x, ascent, descent, first);
}

std::string_view listValue(std::string_view values, unsigned index) {
  values = trim(values);
  while (!values.empty()) {
    const auto end = values.find_first_of(" \t\r\n");
    if (!index || end == values.npos) return values.substr(0, end);
    --index;
    values = trim(values.substr(end));
  }
  return {};
}

Box Layout::table(const Node& node, Style style, int depth) {
  constexpr unsigned MAX_COLUMNS = 16;
  const auto first = drawing_.commandCount;
  style.display = attr(node, "displaystyle") == "true";
  const auto columnSpan = [&](uint16_t index) {
    return static_cast<unsigned>(std::clamp(attrInt(nodes_[index], "columnspan", 1), 1, static_cast<int>(MAX_COLUMNS)));
  };
  // Measure cells before arranging the grid. The same box array holds all
  // row and cell metrics, and the fixed column arrays cost 128 stack bytes.
  std::array<int, MAX_COLUMNS> widths{};
  unsigned columnCount = 0;
  int labels = 0;
  for (auto rowIndex = node.firstChild; rowIndex != NO_NODE && !failed_; rowIndex = nodes_[rowIndex].nextSibling) {
    const auto& rowNode = nodes_[rowIndex];
    const bool actualRow = rowNode.name == "mtr" || rowNode.name == "mlabeledtr";
    const auto rowStart = drawing_.commandCount;
    unsigned column = 0;
    int ascent = 0;
    int descent = 0;
    bool label = rowNode.name == "mlabeledtr";
    const auto rowStyle = styled(rowNode, style);
    for (auto cell = actualRow ? rowNode.firstChild : rowIndex; cell != NO_NODE && !failed_;
         cell = actualRow ? nodes_[cell].nextSibling : NO_NODE) {
      const auto box = measure(cell, rowStyle, depth);
      ascent = std::max<int>(ascent, box.ascent);
      descent = std::max<int>(descent, box.descent);
      if (label) {
        labels = std::max<int>(labels, box.width);
        label = false;
        continue;
      }
      const auto span = columnSpan(cell);
      if (column + span > MAX_COLUMNS || attrInt(nodes_[cell], "rowspan", 1) != 1) {
        failed_ = true;
        break;
      }
      int currentWidth = 0;
      for (unsigned i = 0; i < span; ++i) currentWidth += widths[column + i];
      int extra = std::max(0, static_cast<int>(box.width) - currentWidth);
      for (unsigned i = 0; i < span; ++i) {
        const int increment = extra / static_cast<int>(span - i);
        widths[column + i] += increment;
        extra -= increment;
      }
      column += span;
    }
    if (actualRow) boxes_[rowIndex] = bounds(0, ascent, descent, rowStart);
    columnCount = std::max(columnCount, column);
  }
  if (failed_ || !columnCount) return {};
  if (attr(node, "equalcolumns") == "true") {
    const int largest = *std::max_element(widths.begin(), widths.end());
    widths.fill(largest);
  }
  const bool frame = !attr(node, "frame").empty() && attr(node, "frame") != "none";
  const int framePad = frame ? std::max(2, style.em / 4) : 0;
  const int labelSpace = labels ? length(attr(node, "minlabelspacing"), style.em, style.em / 2) + labels : 0;
  const bool labelLeft = attr(node, "side") == "left";
  const int gridX = framePad + (labelLeft ? labelSpace : 0);
  std::array<int, MAX_COLUMNS> starts{};
  int right = gridX;
  for (unsigned column = 0; column < columnCount; ++column) {
    starts[column] = right;
    right += widths[column];
    if (column + 1 != columnCount) {
      right += std::max(0, length(listValue(attr(node, "columnspacing"), column), style.em, style.em / 2));
    }
  }
  const int width = right + framePad + (labelLeft ? 0 : labelSpace);
  int y = framePad;
  unsigned rowNumber = 0;
  for (auto rowIndex = node.firstChild; rowIndex != NO_NODE && !failed_;
       rowIndex = nodes_[rowIndex].nextSibling, ++rowNumber) {
    const auto& rowNode = nodes_[rowIndex];
    const auto& rowBox = boxes_[rowIndex];
    const bool actualRow = rowNode.name == "mtr" || rowNode.name == "mlabeledtr";
    unsigned column = 0;
    bool label = rowNode.name == "mlabeledtr";
    const int baseline = y + rowBox.ascent;
    for (auto cell = actualRow ? rowNode.firstChild : rowIndex; cell != NO_NODE;
         cell = actualRow ? nodes_[cell].nextSibling : NO_NODE) {
      const auto& box = boxes_[cell];
      if (label) {
        move(box, labelLeft ? labels - box.width : width - labels, baseline);
        label = false;
        continue;
      }
      const auto span = columnSpan(cell);
      const int available = starts[column + span - 1] + widths[column + span - 1] - starts[column];
      auto align = attr(nodes_[cell], "columnalign");
      if (align.empty()) align = listValue(attr(rowNode, "columnalign"), column);
      if (align.empty()) align = listValue(attr(node, "columnalign"), column);
      const int x = align == "left" ? 0 : align == "right" ? available - box.width : (available - box.width) / 2;
      const auto rowAlign = attr(nodes_[cell], "rowalign");
      const int cellY = rowAlign == "top"      ? y + box.ascent
                        : rowAlign == "bottom" ? y + rowBox.ascent + rowBox.descent - box.descent
                                               : baseline;
      move(box, starts[column] + x, cellY);
      column += span;
    }
    y = baseline + rowBox.descent;
    if (rowNode.nextSibling != NO_NODE) {
      const int gap = std::max(0, length(listValue(attr(node, "rowspacing"), rowNumber), style.em, style.em / 2));
      const auto line = listValue(attr(node, "rowlines"), rowNumber);
      if (!line.empty() && line != "none") rule(gridX, y + gap / 2, right - 1, y + gap / 2);
      y += gap;
    }
  }
  y += framePad;
  for (unsigned column = 0; column + 1 < columnCount; ++column) {
    const auto line = listValue(attr(node, "columnlines"), column);
    const int x = (starts[column] + widths[column] + starts[column + 1]) / 2;
    if (!line.empty() && line != "none") rule(x, framePad, x, y - framePad);
  }
  if (frame) {
    const int leftEdge = gridX - framePad;
    const int rightEdge = right + framePad - 1;
    rule(leftEdge, 0, rightEdge, 0);
    rule(leftEdge, y - 1, rightEdge, y - 1);
    rule(leftEdge, 0, leftEdge, y - 1);
    rule(rightEdge, 0, rightEdge, y - 1);
  }
  const auto align = listValue(attr(node, "align"), 0);
  const int baseline = align == "top"      ? 0
                       : align == "bottom" ? y
                       : align == "center" ? y / 2
                                           : y / 2 + metric(Constant::AxisHeight, style.em);
  auto result = bounds(width, baseline, y - baseline, first);
  move(result, 0, -baseline);
  return result;
}

Box Layout::enclosure(const Node& node, Style style, int depth) {
  const auto first = drawing_.commandCount;
  const auto body = row(node, style, depth, false);
  auto notations = attr(node, "notation");
  if (notations.empty()) notations = node.name == "merror" ? "box" : "longdiv";
  const int pad = std::max(2, style.em / 7);
  const int width = body.width + 2 * pad;
  const int top = -body.ascent - pad;
  const int bottom = body.descent + pad;
  const int right = width - 1;
  move(body, pad, 0);
  while (!notations.empty() && !failed_) {
    const auto notation = listValue(notations, 0);
    notations = trim(notations.substr(notation.size()));
    const bool box = notation == "box" || notation == "roundedbox";
    if (box || notation == "top" || notation == "actuarial" || notation == "longdiv") rule(0, top, right, top);
    if (box || notation == "bottom" || notation == "madruwb") rule(0, bottom, right, bottom);
    if (box || notation == "left") rule(0, top, 0, bottom);
    if (box || notation == "right" || notation == "actuarial" || notation == "madruwb") rule(right, top, right, bottom);
    if (notation == "horizontalstrike") rule(0, (top + bottom) / 2, right, (top + bottom) / 2);
    if (notation == "verticalstrike") rule(right / 2, top, right / 2, bottom);
    if (notation == "updiagonalstrike" || notation == "updiagonalarrow") rule(0, bottom, right, top);
    if (notation == "downdiagonalstrike") rule(0, top, right, bottom);
    if (notation == "updiagonalarrow") {
      rule(right - pad, top, right, top);
      rule(right, top, right, top + pad);
    }
    if (notation == "longdiv") {
      rule(0, top, pad / 2, (top + bottom) / 2);
      rule(pad / 2, (top + bottom) / 2, 0, bottom);
    }
    if (notation == "circle") {
      const int xInset = std::max(1, width / 6);
      const int yInset = std::max(1, (bottom - top) / 6);
      rule(xInset, top, right - xInset, top);
      rule(xInset, bottom, right - xInset, bottom);
      rule(0, top + yInset, 0, bottom - yInset);
      rule(right, top + yInset, right, bottom - yInset);
      rule(0, top + yInset, xInset, top);
      rule(0, bottom - yInset, xInset, bottom);
      rule(right, top + yInset, right - xInset, top);
      rule(right, bottom - yInset, right - xInset, bottom);
    }
  }
  return bounds(width, -top, bottom + 1, first);
}

Box Layout::stack(const Node& node, Style style, int depth) {
  const auto first = drawing_.commandCount;
  int width = 0;
  int baseline = 0;
  int bottom = 0;
  int ascent = 0;
  bool firstRow = true;
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    const auto box = measure(child, style, depth);
    width = std::max<int>(width, box.width);
  }
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    const auto& box = boxes_[child];
    if (nodes_[child].name == "msline") {
      rule(0, bottom + 1, std::max(0, width - 1), bottom + 1);
      bottom += 3;
      continue;
    }
    baseline = firstRow ? 0 : bottom + box.ascent + std::max(1, style.em / 7);
    if (firstRow) ascent = box.ascent;
    firstRow = false;
    const int shift = attrInt(nodes_[child], "position", 0) * std::max(1, style.em / 2);
    move(box, std::max(0, width - box.width - shift), baseline);
    bottom = baseline + box.descent;
  }
  if (node.name == "mlongdiv" && width > 0) {
    const int x = std::max(0, width - boxes_[node.firstChild].width - 2);
    rule(x, -ascent, x, bottom);
    rule(x, -ascent, width - 1, -ascent);
  }
  return bounds(width, ascent, bottom, first);
}

Box Layout::mixedToken(const Node& node, Style style, int depth, Form form) {
  const auto first = drawing_.commandCount;
  const bool quoted = node.name == "ms";
  int width = 0;
  int ascent = 0;
  int descent = 0;
  const auto place = [&](const Box& item) {
    move(item, width, 0);
    width += item.width;
    ascent = std::max<int>(ascent, item.ascent);
    descent = std::max<int>(descent, item.descent);
  };
  if (quoted) place(readerText(attribute(node, "lquote").value_or("\""), style.em));
  place(token(node, trim(node.text), style, form, 0, Direction::Vertical));
  for (auto child = node.firstChild; child != NO_NODE && !failed_; child = nodes_[child].nextSibling) {
    const auto& item = nodes_[child];
    place(item.name == "#text" ? token(node, item.text, style, form, 0, Direction::Vertical)
                               : measure(child, style, depth));
  }
  if (quoted) place(readerText(attribute(node, "rquote").value_or("\""), style.em));
  return bounds(width, ascent, descent, first);
}

Box Layout::padded(const Node& node, Style style, int depth) {
  const auto body = row(node, style, depth, false);
  const auto dimension = [&](std::string_view key, int base) {
    const auto value = attr(node, key);
    const bool relative = value.starts_with('+') || value.starts_with('-');
    return (relative ? base : 0) + length(value, style.em, relative ? 0 : base, base, &body);
  };
  move(body, length(attr(node, "lspace"), style.em, 0, body.width, &body),
       -length(attr(node, "voffset"), style.em, 0, body.ascent, &body));
  return bounds(dimension("width", body.width), dimension("height", body.ascent), dimension("depth", body.descent),
                body.first);
}

Box Layout::measure(uint16_t index, Style style, int depth, Form form, int stretch, Direction direction, bool wrap) {
  if (depth >= MAX_DEPTH || failed_) {
    failed_ = true;
    return {};
  }
  const auto& node = nodes_[index];
  const auto name = node.name;
  // RV32 stack-usage measurements include about 256 B of dispatch plus the
  // active helper. Budget table, scripts, and terminal glyph lookup separately
  // so a nested table cannot consume the loop task's full stack.
  const int frameBytes = name == "mtable" ? 832
                         : name == "mi" || name == "mo" || name == "mn" || name == "mtext" || name == "ms" ||
                                 name == "#text" || name == "mglyph"
                             ? 960
                         : name == "munder" || name == "mover" || name == "munderover" || name == "msub" ||
                                 name == "msup" || name == "msubsup"
                             ? 640
                             : 544;
  if (stackBytes_ + frameBytes > MAX_LAYOUT_STACK_BYTES) {
    failed_ = true;
    return {};
  }
  stackBytes_ += frameBytes;
  const auto first = drawing_.commandCount;
  const auto textStart = drawing_.textBytes;
  style = styled(node, style);
  ++depth;
  Box result;

  if (name == "math" && node.firstChild != NO_NODE && nodes_[node.firstChild].name == "mrow" &&
      nodes_[node.firstChild].nextSibling == NO_NODE) {
    result = measure(node.firstChild, style, depth, form, stretch, direction, wrap);
  } else if (name == "math" || name == "mrow" || name == "mstyle" || name == "mtd" || name == "mtr" ||
             name == "mlabeledtr" || name == "msrow" || name == "mscarry") {
    result = row(node, style, depth, wrap);
  } else if (name == "mi" || name == "mn" || name == "mo" || name == "mtext" || name == "ms" || name == "#text") {
    if (name == "ms" || node.firstChild != NO_NODE) {
      result = mixedToken(node, style, depth, form);
    } else {
      result = token(node, tokenText(node), style, form, stretch, direction);
    }
  } else if (name == "mglyph") {
    result = readerText(attr(node, "alt"), style.em);
  } else if (name == "mspace") {
    result = bounds(length(attr(node, "width"), style.em, 0), length(attr(node, "height"), style.em, 0),
                    length(attr(node, "depth"), style.em, 0), first);
  } else if (name == "mfrac") {
    result = fraction(node, style, depth);
  } else if (name == "msqrt" || name == "mroot") {
    result = radical(node, style, depth);
  } else if (name == "msub" || name == "msup" || name == "msubsup" || name == "munder" || name == "mover" ||
             name == "munderover") {
    result = scripts(node, style, depth, form);
  } else if (name == "mmultiscripts") {
    result = multiscripts(node, style, depth, form);
  } else if (name == "mfenced") {
    result = fenced(node, style, depth);
  } else if (name == "mtable") {
    result = table(node, style, depth);
  } else if (name == "menclose" || name == "merror") {
    result = enclosure(node, style, depth);
  } else if (name == "mstack" || name == "mlongdiv" || name == "msgroup" || name == "mscarries") {
    result = stack(node, style, depth);
  } else if (name == "mpadded") {
    result = padded(node, style, depth);
  } else if (name == "mphantom") {
    const auto body = row(node, style, depth, false);
    drawing_.commandCount = first;
    drawing_.textBytes = textStart;
    result = bounds(body.width, body.ascent, body.descent, first);
  } else if (name == "semantics" || name == "maction") {
    auto child = node.firstChild;
    int selected = name == "maction" ? std::max(1, attrInt(node, "selection", 1)) : 1;
    while (child != NO_NODE && --selected > 0) child = nodes_[child].nextSibling;
    if (child == NO_NODE) child = node.firstChild;
    if (child != NO_NODE) result = measure(child, style, depth, form, stretch, direction, wrap);
  } else if (name != "annotation" && name != "annotation-xml" && name != "none" && name != "mprescripts" &&
             name != "maligngroup" && name != "malignmark" && name != "msline") {
    failed_ = true;
  }
  boxes_[index] = result;
  stackBytes_ -= frameBytes;
  return result;
}

bool Layout::run(bool display) {
  if (!font_.valid()) return false;
  int em = baseEm_;
  for (int attempt = 0; attempt < 6; ++attempt) {
    drawing_.commandCount = 0;
    drawing_.textBytes = 0;
    failed_ = false;
    const auto box = measure(0, {em, 0, display, false, {}}, 0, Form::Infix, 0, Direction::Vertical, display);
    if (failed_) return false;
    const int height = box.ascent + box.descent;
    if (box.width <= width_ && height <= height_) {
      move(box, 0, box.ascent);
      if (failed_) return false;
      drawing_.width = std::max(0, static_cast<int>(box.width));
      drawing_.ascent = box.ascent;
      drawing_.descent = box.descent;
      return true;
    }
    if (em <= 6) return false;
    const int widthEm = em * width_ / std::max(1, static_cast<int>(box.width));
    const int heightEm = em * height_ / std::max(1, height);
    em = std::max(6, std::min({em - 1, widthEm, heightEm}));
  }
  return false;
}

}  // namespace

bool layout(std::span<const Node> nodes, GfxRenderer& renderer, int fontId, uint16_t maxWidth, uint16_t maxHeight,
            uint8_t fontSize, bool display, Drawing& result) {
  if (nodes.empty() || nodes.size() > MAX_NODES || !maxWidth || !maxHeight) return false;
  size_t sourceBytes = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& node = nodes[i];
    for (const auto index : {node.firstChild, node.nextSibling}) {
      if (index != NO_NODE && (index <= i || index >= nodes.size())) return false;
    }
    sourceBytes += node.text.size();
  }
  const auto commandCapacity =
      static_cast<uint16_t>(std::min<size_t>(MAX_LAYOUT_COMMANDS, nodes.size() * 3 + sourceBytes + 16));
  const auto textCapacity = static_cast<uint16_t>(std::min<size_t>(MAX_LAYOUT_TEXT, sourceBytes * 2 + 32));

  // Drawings outlive layout and need independently owned buffers. Use the
  // formula size to avoid reserving the 5.3 KiB worst case for short equations.
  Drawing drawing;
  drawing.commands = makeUniqueNoThrow<DrawCommand[]>(commandCapacity);
  drawing.text = makeUniqueNoThrow<char[]>(textCapacity);
  drawing.fontSize = fontSize;
  // The 1.9 KiB worst-case box array and ~3.5 KiB font workspace cannot be
  // held on the parser stack. Allocate both only while laying out a formula.
  auto boxes = makeUniqueNoThrow<Box[]>(nodes.size());
  auto engine = makeUniqueNoThrow<Layout>(nodes, renderer, fontId, fontSize, std::min(maxWidth, Drawing::MAX_DIMENSION),
                                          std::min(maxHeight, Drawing::MAX_DIMENSION), drawing, boxes.get(),
                                          commandCapacity, textCapacity);
  if (!drawing.commands || !drawing.text || !boxes || !engine) {
    LOG_ERR("MML", "Could not allocate formula layout buffers");
    return false;
  }
  if (!engine->run(display)) return false;
  result = std::move(drawing);
  return true;
}

}  // namespace mathml
