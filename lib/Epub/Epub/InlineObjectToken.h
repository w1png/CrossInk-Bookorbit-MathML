#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

enum class InlineObjectKind : uint8_t { Image, Math };

struct InlineObjectToken {
  uint16_t id;
  uint16_t width;
  uint16_t height;
  uint16_t ascent;
  InlineObjectKind kind;
};

struct InlineObjectPlacement {
  uint16_t id;
  int16_t x;
  uint16_t height;
  uint16_t ascent;
  InlineObjectKind kind;
};

struct InlineLineMetrics {
  int height;
  int textTop;
  int baseline;
};

inline InlineLineMetrics measureInlineLine(const std::span<const InlineObjectPlacement> objects, const int textHeight,
                                           const int textBaseline, const bool includeImages = true) {
  int above = textBaseline;
  int below = textHeight - textBaseline;
  int imageHeight = 0;
  for (const auto& object : objects) {
    if (object.kind == InlineObjectKind::Math) {
      above = std::max(above, static_cast<int>(object.ascent));
      below = std::max(below, static_cast<int>(object.height) - object.ascent);
    } else if (includeImages) {
      imageHeight = std::max(imageHeight, static_cast<int>(object.height));
    }
  }
  const int height = std::max(above + below, imageHeight);
  const int baseline = height - below;
  return {height, baseline - textBaseline, baseline};
}

// Layout-only token removed before constructing TextBlock. Keep both formats
// within the ESP32's 15-byte std::string small buffer. Math uses three hex
// digits for each dimension, sufficient for the bounded math drawing.
inline std::string makeInlineImageToken(const uint16_t id, const uint16_t width, const uint16_t height) {
  char token[15];
  snprintf(token, sizeof(token), "\x01I%04X%04X%04X", id, width, height);
  return token;
}

inline std::string makeInlineMathToken(const uint16_t id, const uint16_t width, const uint16_t height,
                                       const uint16_t ascent) {
  char token[16];
  snprintf(token, sizeof(token), "\x01M%04X%03X%03X%03X", id, width, height, ascent);
  return token;
}

inline bool isInlineMathToken(const std::string_view token) {
  return token.size() == 15 && token[0] == '\x01' && token[1] == 'M';
}

inline bool isInlineObjectToken(const std::string_view token) {
  return (token.size() == 14 && token[0] == '\x01' && token[1] == 'I') || isInlineMathToken(token);
}

inline bool isClosingPunctuationForWrap(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

inline bool parseInlineObjectToken(const std::string_view token, InlineObjectToken& object) {
  if (!isInlineObjectToken(token)) return false;
  const bool isMath = token[1] == 'M';
  uint16_t fields[4] = {};
  size_t pos = 2;
  for (int field = 0; field < (isMath ? 4 : 3); ++field) {
    const int digits = isMath && field > 0 ? 3 : 4;
    for (int digit = 0; digit < digits; ++digit) {
      const char c = token[pos++];
      if (c >= '0' && c <= '9') {
        fields[field] = static_cast<uint16_t>((fields[field] << 4) | (c - '0'));
      } else if (c >= 'A' && c <= 'F') {
        fields[field] = static_cast<uint16_t>((fields[field] << 4) | (c - 'A' + 10));
      } else {
        return false;
      }
    }
  }
  if ((!isMath && (fields[1] == 0 || fields[2] == 0)) || fields[3] > fields[2]) return false;
  object = {fields[0], fields[1], fields[2], fields[3], isMath ? InlineObjectKind::Math : InlineObjectKind::Image};
  return true;
}
