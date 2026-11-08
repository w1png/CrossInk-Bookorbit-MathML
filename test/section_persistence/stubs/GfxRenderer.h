#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <utility>
#include <vector>

class FontCacheManager;

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
  int getFontIdForSize(int id, uint8_t) const { return id; }
  int getFontAscenderSize(int) const { return 7; }
  bool isFontCacheScanning() const { return false; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char*, EpdFontFamily::Style) const { return 8; }
  template <class BaseDir>
  int getTextWidth(int, const char*, EpdFontFamily::Style, BaseDir) const {
    return 8;
  }
  template <class... Args>
  void drawLine(Args...) const {}
  template <class... Args>
  void drawText(Args...) const {}
  template <class... Args>
  void fillRect(Args...) const {}
  void drawRect(int, int, int, int, bool) const {}
  void beginTextClip(int, int, int, int) const {}
  void endTextClip() const {}

  std::vector<std::pair<int, int>> mathOrigins;
};
