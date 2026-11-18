// HOST STUB for tools/theme_tokens/check_theme_syntax.sh. NOT part of the firmware
// build: it lives under tools/ and is only ever reached through that script's -I.
// Its job is to let a theme .cpp be type-checked on the host against the REAL
// BaseTheme.h; every renderer signature below is copied from the real header.

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Bitmap.h"
#include "EpdFontFamily.h"
// Signatures copied verbatim from lib/GfxRenderer/GfxRenderer.h.
class GfxRenderer {
 public:
  int getScreenHeight() const;
  int getScreenWidth() const;
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
};
