#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontPointSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  uint8_t paragraphIndentSpaces = 2;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  bool focusReading = false;
  bool hyphenation = false;
  bool operator==(const PreviewKey&) const = default;
};

// Cached engine preview lines + the key that produced them
struct PreviewLayout {
  PreviewLayout();
  ~PreviewLayout();

  std::vector<std::unique_ptr<TextBlock>> lines;
  PreviewKey key;
};

// Draws the sample-text pane via the reader engine, reusing layout across redraws.
//
// C1 (upstream 3f387a98): the pane is positioned by BOUNDS, never by
// renderer.getScreenWidth(). On a rotated panel the usable width is the safe
// area's, and deriving it from the screen width put the sample text under the
// button-hint band and off the bezel-cleared region.
void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int left,
                   int top, int width, int height, const char* familyName, const char* sizeName);

}  // namespace textsettings
