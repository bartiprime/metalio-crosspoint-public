#include "GfxRenderer.h"

// r97: PlatformFeatures.h reached this file only TRANSITIVELY, through platform/CpuStats.h.
// Every CP_ switch below (CP_RICKY_REFRESH_POLICY, CP_CPU_STATS, CP_WAKE_ONE_DRIVE, ...) would
// then silently evaluate to 0 the day that include moves inside CpuStats.h's own #if CP_CPU_STATS
// block -- and an undefined identifier in #if is 0 in C/C++, so the whole RickyOS refresh policy
// would compile out with no error. Include it directly: this file's behaviour depends on it.
#include "platform/PlatformFeatures.h"
#include "platform/CpuStats.h"

#include <BidiUtils.h>
#include <BoardConfig.h>
#include <BuildScratch.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <MemoryManager.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Memory/Memory.h"
#include "../../src/display/RefreshPolicy.h"
// EXP1/EDRV1: the `exp row9` arm. With CP_EXP=OFF this header supplies a
// constexpr completionDrive() == true, so this call site is the pre-batch one.
#include "platform/ExpArms.h"
#include "FontCacheManager.h"
#include "GlyphBitmap.h"

namespace {
// Synthesized small caps: lowercase letters draw as capitals at roughly x-height.
constexpr int SMALL_CAPS_SCALE_NUM = 3;
constexpr int SMALL_CAPS_SCALE_DEN = 4;
constexpr int32_t scaleSmallCapAdvance(const int32_t advanceFP) {
  return (advanceFP * SMALL_CAPS_SCALE_NUM + SMALL_CAPS_SCALE_DEN / 2) / SMALL_CAPS_SCALE_DEN;
}
}  // namespace

namespace {
constexpr int trackingBetween(const uint32_t leftCp, const uint32_t rightCp, const int8_t tracking) {
  const auto isSpace = [](const uint32_t cp) { return cp == ' ' || cp == 0xA0 || cp == 0x3000; };
  return leftCp == 0 || isSpace(leftCp) || isSpace(rightCp) ? 0 : tracking;
}

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}

uint16_t getSdCardSpaceAdvance(SdCardFont& font, const EpdFontFamily::Style style) {
  const uint8_t resolvedStyle = resolveSdCardStyle(font, style);
  const uint16_t advance = font.getAdvance(' ', resolvedStyle);
  if (advance != 0) return advance;

  // Zero means uncached (full table, style not prewarmed, or failed
  // preparation): read the glyph, as the per-codepoint slow path does.
  const EpdFont* epdFont = font.getEpdFont(resolvedStyle);
  const EpdGlyph* glyph = epdFont ? epdFont->getGlyph(' ') : nullptr;
  return glyph ? glyph->advanceX : 0;
}
}  // namespace

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, BidiUtils::BidiBaseDir baseDir);

// Appends the shaped visual form of every RTL token in `text` to `shapedOut`.
// getTextAdvanceX() measures the bidi-reordered, Arabic-shaped codepoint stream,
// so the SD advance table must be warmed with the presentation forms as well as
// the logical codepoints — otherwise every RTL word measurement misses the fast
// path and falls through to onGlyphMiss(), which opens the .cpfont and reads
// glyph metadata + bitmap into the 8-slot overflow ring, once per glyph.
// Tokens without RTL lead bytes (0xD6-0xDB) are skipped with a byte scan, so
// pure-LTR text pays almost nothing.
void appendShapedRtlTokens(const char* text, std::string& shapedOut) {
  const auto isBreak = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  std::string token;
  std::string visual;
  const char* p = text;
  while (*p) {
    while (*p && isBreak(*p)) ++p;
    const char* start = p;
    bool hasRtlBytes = false;
    while (*p && !isBreak(*p)) {
      const auto b = static_cast<unsigned char>(*p);
      hasRtlBytes = hasRtlBytes || (b >= 0xD6 && b <= 0xDB);
      ++p;
    }
    if (!hasRtlBytes) continue;
    token.assign(start, p - start);
    if (BidiUtils::applyBidiVisual(token.c_str(), visual, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
      shapedOut += visual;
    }
  }
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  // Vector (TTF) fonts: the glyph bitmap lives in the font's own cache, keyed by
  // the EpdGlyph the miss handler returned. Checked first so it never reaches the
  // SdCardFont overflow cast below. nullptr = zero-width glyph (e.g. space).
  if (fontData->vectorBitmapHandler != nullptr) {
    return fontData->vectorBitmapHandler(fontData->glyphMissCtx, glyph);
  }
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    std::string shaped;
    appendShapedRtlTokens(utf8Text, shaped);
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
    return;
  }
  // TTF (vector) fonts need nothing here: getGlyph faults glyphs in on demand
  // (glyphMissHandler), and the page's set is batch-warmed by the render scan's
  // prewarmCache(). Pre-faulting a whole chapter's text during layout would just
  // thrash the page-bounded cache, so it is intentionally omitted.
}

void GfxRenderer::ensureSdCardFontReady(const int fontId, const char* const* segments, const size_t* segmentLens,
                                        const size_t segmentCount, const bool includeSpace, const bool includeHyphen,
                                        const uint8_t styleMask, const bool includeSmallCaps) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    std::string shaped;
    for (size_t seg = 0; seg < segmentCount; seg++) {
      const char* p = segments[seg];
      const char* const end = p + segmentLens[seg];
      while (p < end) {
        appendShapedRtlTokens(p, shaped);
        p += strlen(p) + 1;
      }
    }
    int missed =
        it->second->buildAdvanceTablePacked(segments, segmentLens, segmentCount, includeSpace, includeHyphen, styleMask,
                                            shaped.empty() ? nullptr : shaped.c_str(), includeSmallCaps);
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
    return;
  }
  // TTF (vector) fonts: nothing to do — glyphs fault in on demand at getGlyph
  // and the page is batch-warmed by the render scan (see the other overload).
}

void GfxRenderer::setContentRole(const ContentRole role) const {
  if (contentRole_ == role) return;
  // r163 D3: the ONE point a role is applied -- ScopedContentRole routes every tag through
  // here -- so this is where the admission rule becomes readable. DBG, and only on a change:
  // a per-blit line would bury the log, and a role restored at the end of a block names the
  // previous role again, which is exactly the scoping we want to see.
  LOG_DBG("GFX", "content role: %s -> %s (grey %s)", gfxcontent::nameOf(contentRole_), gfxcontent::nameOf(role),
          roleAdmitsGrey(role) ? "admitted" : "denied by role");
  contentRole_ = role;
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);

  // Evictable-cache sinks for the SDK memory manager: layout's OOM choke
  // points (WordStore/TextBlock) call ensureFree() to flush these and retry
  // before failing a section build. Both caches rebuild transparently from SD
  // or flash on the next glyph access. The SD-font persistent advance table is
  // deliberately NOT a sink: evicting it mid-paragraph would make later
  // measurements in the same layout pass return 0-width advances.
  auto& memoryManager = freeink::MemoryManager::instance();
  memoryManager.registerSink({"gfx.renderGlyphCache", 50, [this](size_t) -> size_t {
                                if (!fontCacheManager_ || fontCacheManager_->isScanning()) return 0;
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                fontCacheManager_->clearCache();
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
  memoryManager.registerSink({"gfx.sdFontMini", 60, [this](size_t) -> size_t {
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                for (auto& entry : sdCardFonts_) {
                                  if (entry.second) entry.second->clearCache();
                                }
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
#if CROSSPOINT_VECTOR_FONTS
  // TTF glyph arenas are safe to shed mid-layout, unlike the SD-font advance
  // table: metrics re-fault through FreeType with identical values, so layout
  // cannot silently corrupt — the cost is re-rasterizing on the next draw.
  memoryManager.registerSink({"gfx.ttfGlyphArenas", 70, [this](size_t) -> size_t {
                                const size_t before = freeink::MemoryManager::instance().freeBytes();
                                for (auto& entry : ttfFonts_) {
                                  if (entry.second) entry.second->releaseResidentCaches();
                                }
                                const size_t after = freeink::MemoryManager::instance().freeBytes();
                                return after > before ? after - before : 0;
                              }});
#endif
}

void GfxRenderer::releaseFrameBufferForBuild() {
  // Lend the framebuffer's bytes IN PLACE: the allocation is never freed, so
  // it cannot move and repeated loans cannot fragment the heap (the previous
  // free+realloc model measurably decayed the max contiguous block over a
  // session). The bytes are deposited in the build-scratch registry so
  // memory-hungry build phases (e.g. InflateStream's tinfl state + window)
  // can claim them instead of allocating.
  uint32_t size = 0;
  uint8_t* scratch = display.lendFrameBufferStorage(&size);
  frameBuffer = nullptr;
  frameBufferLoans++;
  if (scratch) {
    buildscratch::lend(scratch, size);
  }
}

bool GfxRenderer::restoreFrameBufferAfterBuild() {
  buildscratch::reclaim();
  display.returnFrameBufferStorage();  // cannot fail: the allocation was never freed
  frameBuffer = display.getFrameBuffer();
  return frameBuffer != nullptr;
}

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer) {
  // Nesting guard: if the framebuffer is already lent out (an outer loan),
  // stay inert so this end() cannot return storage the outer loan still owns.
  if (!renderer_.hasFrameBuffer()) return;
  renderer_.releaseFrameBufferForBuild();
  active_ = true;
}

void GfxRenderer::FrameBufferLoan::end() {
  if (!active_) return;
  active_ = false;
  if (!renderer_.restoreFrameBufferAfterBuild()) {
    // Only reachable if the framebuffer never existed, which begin() already
    // asserts against; kept as a backstop since running blind helps nobody.
    LOG_ERR("GFX", "Framebuffer restore failed - restarting");
    ESP.restart();
  }
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

int GfxRenderer::resolveTextFontId(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (fallbackFontMap_.empty() || text == nullptr || *text == '\0') {
    return fontId;
  }
  const auto fbIt = fallbackFontMap_.find(fontId);
  if (fbIt == fallbackFontMap_.end()) {
    return fontId;  // no fallback registered for this font
  }
  const int fallbackFontId = fbIt->second;
  const auto fontIt = fontMap.find(fontId);
  const auto fallbackIt = fontMap.find(fallbackFontId);
  if (fontIt == fontMap.end() || fallbackIt == fontMap.end()) {
    return fontId;  // unknown primary or fallback not loaded — let the caller handle it
  }
  const EpdFontFamily& primary = fontIt->second;
  const EpdFontFamily& fallback = fallbackIt->second;
  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    // Redirect whenever the primary font cannot draw a codepoint the fallback
    // can — otherwise the string renders with holes. Not just CJK: Greek,
    // Cyrillic, or any script the built-in UI fonts lack qualifies. ASCII is
    // skipped outright (every registered font covers it), keeping Latin
    // strings on the fast path, and a partial-coverage fallback is not worth
    // dragging the whole string into for glyphs it would also miss.
    if (cp >= 0x80 && !primary.hasCodepoint(cp, style) && fallback.hasCodepoint(cp, style)) {
      return fallbackFontId;
    }
  }
  return fontId;
}

void GfxRenderer::prewarmFallbackText(const int fontId, const TextGetter getter, const void* ctx,
                                      const uint32_t textCount, const EpdFontFamily::Style style) const {
  if (getter == nullptr || textCount == 0) {
    return;
  }
  // Resolve the fallback id from the first string that actually redirects; a
  // screen with no non-Latin strings resolves nothing and this is a no-op.
  int fallbackFontId = fontId;
  for (uint32_t i = 0; i < textCount && fallbackFontId == fontId; i++) {
    const char* text = getter(ctx, i);
    if (text == nullptr || *text == '\0') continue;
    fallbackFontId = resolveTextFontId(fontId, text, style);
  }
  if (fallbackFontId == fontId) {
    return;
  }
  const auto sdIt = sdCardFonts_.find(fallbackFontId);
  if (sdIt == sdCardFonts_.end()) {
    return;
  }
  const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
  // Append one virtual index for U+2026: truncation measures every long row
  // as "label…", so an ellipsis missing from the batch forces a union rebuild
  // on the first repaint.
  struct WrapCtx {
    TextGetter getter;
    const void* ctx;
    uint32_t count;
  } wrap{getter, ctx, textCount};
  const auto withEllipsis = [](const void* wc, uint32_t i) -> const char* {
    const auto* w = static_cast<const WrapCtx*>(wc);
    return i < w->count ? w->getter(w->ctx, i) : "\xe2\x80\xa6";
  };
  // loadKernLig=false: see ensureSdGlyphsResident below.
  sdIt->second->prewarm(withEllipsis, &wrap, textCount + 1, styleMask, /*metadataOnly=*/false,
                        /*loadKernLig=*/false);
}

void GfxRenderer::prewarmFallbackText(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') {
    return;
  }
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, text, style, false);
  }
}

void GfxRenderer::ensureSdGlyphsResident(const int fontId, const char* text, const EpdFontFamily::Style style,
                                         const bool metadataOnly) const {
  const auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt == sdCardFonts_.end()) {
    return;
  }
  // SUP/SUB bits don't select a distinct .cpfont style bitstream — mask to the
  // base style. resolveStyleMask() inside prewarm folds absent styles.
  // loadKernLig=false: redirected fallback strings (non-Latin titles,
  // filenames) have no useful kern pairs, and the ~3KB class-table load plus
  // per-rebuild mini-matrix build cost heap and SD time exactly where these
  // strings live (heap-tight UI screens). The reader's PrewarmScope path keeps
  // kern; a kern-wanting request that subset-hits a kern-free mini tops the
  // matrix up in prewarmStyle without re-reading glyphs.
  const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
  sdIt->second->prewarm(text, styleMask, metadataOnly, /*loadKernLig=*/false);
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

void GfxRenderer::logicalToPhysical(const Orientation o, const int x, const int y, const uint16_t panelWidth,
                                    const uint16_t panelHeight, int* phyX, int* phyY) {
  rotateCoordinates(o, x, y, phyX, phyY, panelWidth, panelHeight);
}

// Output of screenRectToAlignedMemRect: a rectangle in panel-memory
// coordinates whose x and width are guaranteed to be multiples of 8 (the
// SDK's EInkDisplay::displayWindow alignment requirement). `valid == false`
// means the input was empty or fully outside the panel.
struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

// Translate a screen-coordinate rectangle (the coordinate system used by
// fillRect / drawText / the rest of the renderer's public API) into a
// panel-memory rectangle suitable for direct framebuffer indexing. Rotates
// the rectangle's two opposite corners with rotateCoordinates(), takes the
// bounding box (which naturally swaps width/height in Portrait /
// PortraitInverted), then snaps the x extent outward to multiples of 8 and
// clamps to panel bounds. Precondition: panel dims are multiples of 8 (true
// for the 800x480 panel), so clamping cannot re-break alignment.
static AlignedMemRect screenRectToAlignedMemRect(GfxRenderer::Orientation orientation, int sx, int sy, int sw, int sh,
                                                 uint16_t panelWidth, uint16_t panelHeight) {
  AlignedMemRect out;
  if (sw <= 0 || sh <= 0) return out;

  int x0, y0, x1, y1;
  rotateCoordinates(orientation, sx, sy, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, sx + sw - 1, sy + sh - 1, &x1, &y1, panelWidth, panelHeight);

  const int memXLo = std::min(x0, x1);
  const int memYLo = std::min(y0, y1);
  const int memXHi = std::max(x0, x1) + 1;  // exclusive upper bound
  const int memYHi = std::max(y0, y1) + 1;

  // Snap x outward to multiples of 8.
  int alignedXLo = memXLo & ~0x7;        // round down
  int alignedXHi = (memXHi + 7) & ~0x7;  // round up

  if (alignedXLo < 0) alignedXLo = 0;
  if (alignedXHi > panelWidth) alignedXHi = panelWidth;
  int clampedYLo = memYLo;
  int clampedYHi = memYHi;
  if (clampedYLo < 0) clampedYLo = 0;
  if (clampedYHi > panelHeight) clampedYHi = panelHeight;

  if (alignedXHi <= alignedXLo || clampedYHi <= clampedYLo) return out;

  out.x = static_cast<uint16_t>(alignedXLo);
  out.y = static_cast<uint16_t>(clampedYLo);
  out.w = static_cast<uint16_t>(alignedXHi - alignedXLo);
  out.h = static_cast<uint16_t>(clampedYHi - clampedYLo);
  out.valid = true;
  return out;
}

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph scaled by scaleNum/scaleDen: 1/2 for SUP/SUB, 3/4 for small caps.
//
// Each destination pixel samples the source block it covers. At 1/2, any ink in
// the block draws, preserving thin strokes that nearest-neighbor sampling can skip;
// milder reductions need majority coverage so stems don't thicken.
//
// drawText() scales the advance width by the same factor so layout reserves exactly
// the right horizontal space for the scaled glyph.
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style, const int scaleNum,
                             const int scaleDen) {
  if (renderer.grayPlanesAreAbsolute()) renderMode = GfxRenderer::BW;
  EpdGlyph solidFallback;
  const EpdGlyph* glyph = fontFamily.getGlyphMetrics(cp, solidFallback, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  if (glyph == &solidFallback) {
    renderer.fillRect(cursorX + glyph->left * scaleNum / scaleDen, cursorY - glyph->top * scaleNum / scaleDen,
                      (glyph->width * scaleNum + scaleDen - 1) / scaleDen,
                      (glyph->height * scaleNum + scaleDen - 1) / scaleDen, pixelState);
    return;
  }
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW * scaleNum + scaleDen - 1) / scaleDen;  // ceil so edge columns aren't clipped
  const int dstH = (srcH * scaleNum + scaleDen - 1) / scaleDen;
  // Scale the glyph bearing by the same factor so the scaled glyph sits at the correct
  // pixel offset from the (already-shifted) cursor position.
  const int baseX = cursorX + glyph->left * scaleNum / scaleDen;
  const int baseY = cursorY - glyph->top * scaleNum / scaleDen;
  const bool halving = scaleNum * 2 == scaleDen;
  const bool is2Bit = fontData->is2Bit;

  // Raw sample value: 1-bit fonts read 0 or 3, 2-bit fonts 0 (white) .. 3 (black).
  const auto sample = [&](const int pos) -> uint8_t {
    if (is2Bit) return (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
    return ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

#if CP_CPU_STATS
  // r96: scaled glyphs (SUP/SUB, small caps) are blitted here, not through
  // drawGlyphBitmap, so this path is timed separately — still one pair per glyph.
  const int64_t cpScaledT0 = cpustats::nowUs();
#endif
  for (int dstY = 0; dstY < dstH; dstY++) {
    const int srcY0 = dstY * scaleDen / scaleNum;
    const int srcY1 = std::min(srcH, std::max(srcY0 + 1, ((dstY + 1) * scaleDen + scaleNum - 1) / scaleNum));
    for (int dstX = 0; dstX < dstW; dstX++) {
      const int srcX0 = dstX * scaleDen / scaleNum;
      const int srcX1 = std::min(srcW, std::max(srcX0 + 1, ((dstX + 1) * scaleDen + scaleNum - 1) / scaleNum));
      int coverage = 0;
      uint8_t maxRaw = 0;
      int samples = 0;
      for (int y = srcY0; y < srcY1; y++) {
        for (int x = srcX0; x < srcX1; x++) {
          const uint8_t raw = sample(y * srcW + x);
          coverage += raw;
          maxRaw = std::max(maxRaw, raw);
          samples++;
        }
      }
      if (samples <= 0) continue;
      if constexpr (pixfmt::kFourBit) {
        // Scaled text keeps its coverage: the destination pixel's mean source
        // level (0..3) goes through the SAME 4-level gamma as unscaled glyphs.
        // The 1 bpp path had to threshold here because the plane had nowhere
        // else to put a partial level; the 4 bpp canvas does.
        int level = (coverage + samples / 2) / samples;
        if (level > 3) level = 3;
        const bool keepThin = halving ? (is2Bit ? (maxRaw >= 2 || coverage >= 2) : maxRaw != 0)
                                      : (coverage * 2 >= samples * 3);
        if (keepThin && level < 2) level = 2;
        if (level == 0) continue;
        // r161 MENU AA: same gate as the unscaled blit — a binary paint keeps
        // the old 1 bpp mask look so the frame stays DU-renderable.
        const uint8_t nib = renderer.textBinary() ? pixfmt::kInk
                                                  : pixfmt::glyphLevelToNibble(static_cast<uint8_t>(level));
        renderer.drawPixelLevel(baseX + dstX, baseY + dstY,
                                pixelState ? nib : static_cast<uint8_t>(pixfmt::kPaper - nib));
      } else {
        const bool ink = halving ? (is2Bit ? (maxRaw >= 2 || coverage >= 2) : maxRaw != 0)
                                 : coverage * 2 >= samples * 3;
        if (ink) renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
      }
    }
  }
#if CP_CPU_STATS
  cpustats::noteGlyphUs(cpustats::nowUs() - cpScaledT0);
#endif
}

template <TextRotation rotation = TextRotation::None>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  if (renderer.grayPlanesAreAbsolute()) renderMode = GfxRenderer::BW;
  EpdGlyph solidFallback;
  const EpdGlyph* glyph = fontFamily.getGlyphMetrics(cp, solidFallback, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  if (glyph == &solidFallback) {
    if constexpr (rotation == TextRotation::Rotated90CW) {
      renderer.fillRect(cursorX + fontData->ascender - top, cursorY - left - width + 1, height, width, pixelState);
    } else {
      renderer.fillRect(cursorX + left, cursorY - top, width, height, pixelState);
    }
    return;
  }
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (bitmap == nullptr) return;

  // Logical placement of glyph pixel (0, 0) and the logical step for one move
  // along each glyph axis. Rotated text runs glyph x up the screen and glyph
  // y to the right.
  glyphBitmap::Frame frame;
  if constexpr (rotation == TextRotation::Rotated90CW) {
    frame = {cursorX + fontData->ascender - top, cursorY - left, 0, -1, 1, 0};
  } else {
    frame = {cursorX + left, cursorY - top, 1, 0, 0, 1};
  }
  renderer.drawGlyphBitmap(bitmap, width, height, frame, is2Bit, renderMode, pixelState);
}

// Draw an unscaled glyph placed by a logical frame. Equivalent to calling
// drawPixel() for each ink pixel, but the clip test, orientation rotation,
// strip-band check and address math are resolved once per glyph rather than
// once per pixel.
void GfxRenderer::drawGlyphBitmap(const uint8_t* bitmap, const int width, const int height,
                                  const glyphBitmap::Frame& frame, const bool twoBit, const RenderMode mode,
                                  const bool state) const {
#if CP_CPU_STATS
  // r96 `bench render` GLYPH bucket. ONE esp_timer pair per glyph — never per
  // pixel — so the measurement cannot dominate what it measures.
  const int64_t cpGlyphT0 = cpustats::nowUs();
#endif
  // Apply the logical clip rectangle before rotating, in glyph-local pixels.
  glyphBitmap::Clip clip{0, 0, width, height};
  glyphBitmap::clipToRect(frame, clipLeft_, clipTop_, clipRight_, clipBottom_, clip);

  // Writes go to the framebuffer, or to the strip scratch in tiled grayscale
  // mode; getWriteOriginY()/getWriteRows() bound the rows that exist there.
  // The strip scratch holds 1 bpp controller planes, so it is never 4 bpp.
  glyphBitmap::Target target{getWriteTarget(), panelWidth, panelWidthBytes, getWriteOriginY(), getWriteRows(), {},
                             pixfmt::kFourBit && !_stripActive};
  // r161 MENU AA: binary only on the 4 bpp canvas; the strip scratch is a
  // controller plane and already carries no level.
  // r163 D3: role admission as well as intent -- an `Icon` glyph is binary whatever
  // the paint asked for.
  target.binary = !greyscaleAdmitted();
  // Rotate the glyph origin once, then derive the two physical axes by
  // rotating its neighbours along each logical axis. Together these encode
  // the text rotation and the panel orientation as one orthogonal transform.
  glyphBitmap::Frame& physical = target.frame;
  rotateCoordinates(orientation, frame.x, frame.y, &physical.x, &physical.y, panelWidth, panelHeight);
  int nextX, nextY;
  rotateCoordinates(orientation, frame.x + frame.dxX, frame.y + frame.dxY, &nextX, &nextY, panelWidth, panelHeight);
  physical.dxX = nextX - physical.x;
  physical.dxY = nextY - physical.y;
  rotateCoordinates(orientation, frame.x + frame.dyX, frame.y + frame.dyY, &nextX, &nextY, panelWidth, panelHeight);
  physical.dyX = nextX - physical.x;
  physical.dyY = nextY - physical.y;

  const glyphBitmap::Plane plane = mode == BW              ? glyphBitmap::Plane::BW
                                   : mode == GRAYSCALE_MSB ? glyphBitmap::Plane::GrayMSB
                                                           : glyphBitmap::Plane::GrayLSB;
  glyphBitmap::draw(bitmap, width, height, twoBit, plane, state, target, clip);
#if CP_CPU_STATS
  cpustats::noteGlyphUs(cpustats::nowUs() - cpGlyphT0);
#endif
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  if (x < clipLeft_ || y < clipTop_ || x >= clipRight_ || y >= clipBottom_) return;
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Tiled grayscale: redirect writes to the strip scratch and clip to the
  // current band. Single predictable branch on the hot per-pixel path.
  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  bool strip = false;
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) {
      return;  // pixel outside the band currently being rendered
    }
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
    strip = true;  // the strip scratch always holds 1 bpp controller planes
  }
  // BATT5: this is a canvas mutation (the strip scratch is a controller plane, not
  // the canvas, and is streamed straight to the panel -- it changes nothing that
  // is on the glass after the drive, so it must NOT bump).
  if (!strip) noteFramebufferWrite();

#if CP_RENDER_4BPP
  if (!strip) {
    // 4 bpp canvas: even x is the LOW nibble, matching put4() and epdiy's
    // front_fb, so the panel transfer stays a row copy.
    const uint32_t byteIndex = rowY * panelWidthBytes + (phyX >> 1);
    const uint8_t level = state ? pixfmt::kInk : pixfmt::kPaper;
    if (phyX & 1)
      target[byteIndex] = static_cast<uint8_t>((target[byteIndex] & 0x0F) | (level << 4));
    else
      target[byteIndex] = static_cast<uint8_t>((target[byteIndex] & 0xF0) | level);
    return;
  }
#endif
  // Calculate byte position and bit position
  (void)strip;  // the 1 bpp path below works from rowY alone; the flag is for the 4 bpp branch
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

// Write one explicit level at PHYSICAL panel coordinates. The only clipping
// here is panel bounds plus the active strip band -- both already physical, so
// a caller holding panel coordinates (drawImage's 4 bpp branch, which must land
// exactly where FreeInkDisplay::blitImage puts the 1 bpp path) can use this
// directly. Logical-coordinate drawing APIs arrive through drawPixelLevel(),
// which clips and rotates once before calling in.
void GfxRenderer::commitPixelPhysical(const int phyX, const int phyY, const uint8_t level) const {
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) return;

  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  bool strip = false;
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) return;
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
    strip = true;
  }
  // BATT5: explicit-level writes are canvas mutations too (see drawPixel).
  if (!strip) noteFramebufferWrite();

#if CP_RENDER_4BPP
  if (!strip) {
    // r161 MENU AA: the ONE choke point every explicit level passes through
    // (glyph blits, scaled glyphs, image writers). When the paint was declared
    // binary, nothing intermediate can reach the canvas, so `frameHasGrey` is
    // false in the panel shim and the drive is the binary DU table — regardless
    // of which painter produced the pixel (a 16-level cover thumbnail included).
    // r163 D3: the role is part of the admission, not only the paint's intent.
    const uint8_t written = greyscaleAdmitted() ? level : static_cast<uint8_t>(level < 8 ? pixfmt::kInk : pixfmt::kPaper);
    const uint32_t byteIndex = rowY * panelWidthBytes + (phyX >> 1);
    if (phyX & 1)
      target[byteIndex] = static_cast<uint8_t>((target[byteIndex] & 0x0F) | ((written & 0x0F) << 4));
    else
      target[byteIndex] = static_cast<uint8_t>((target[byteIndex] & 0xF0) | (written & 0x0F));
    return;
  }
#endif
  (void)strip;  // 1 bpp build: the level collapses to an ink/paper bit
  const bool ink = level < 8;
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first
  if (ink) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

// Explicit-level form of drawPixel. Same clip/orientation/strip handling, but
// the value written is the caller's 4 bpp level rather than one of two inks.
// x/y are LOGICAL (screen) coordinates: the rotation happens exactly once, here.
void GfxRenderer::drawPixelLevel(const int x, const int y, const uint8_t level) const {
  if (x < clipLeft_ || y < clipTop_ || x >= clipRight_ || y >= clipBottom_) return;
  int phyX = 0;
  int phyY = 0;
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);
  commitPixelPhysical(phyX, phyY, level);
}

// See the header. 0x00/0xFF/0x0F/0xF0 are the four byte patterns a purely binary
// frame can produce, so any other byte holds an intermediate nibble.
//
// The logical rect is mapped to a PHYSICAL bounding box through rotateCoordinates
// on its corners. Every orientation this renderer supports is a multiple of 90
// degrees, so the mapped region is exactly a rect (no skew, no resampling), and
// the scan is confined to the image's own pixels. Early exit on the first grey
// byte: a photographic image answers on its first row.
bool GfxRenderer::regionCarriesTone(int x, int y, int w, int h) const {
#if CP_RENDER_4BPP
  if constexpr (pixfmt::kFourBit) {
    if (frameBuffer == nullptr || w <= 0 || h <= 0) return false;
    const int corners[4][2] = {{x, y}, {x + w - 1, y}, {x, y + h - 1}, {x + w - 1, y + h - 1}};
    int minX = panelWidth, minY = panelHeight, maxX = -1, maxY = -1;
    for (const auto& c : corners) {
      int px = 0, py = 0;
      rotateCoordinates(orientation, c[0], c[1], &px, &py, panelWidth, panelHeight);
      if (px < minX) minX = px;
      if (py < minY) minY = py;
      if (px > maxX) maxX = px;
      if (py > maxY) maxY = py;
    }
    if (maxX < 0 || maxY < 0 || minX >= panelWidth || minY >= panelHeight) return false;
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX >= panelWidth) maxX = panelWidth - 1;
    if (maxY >= panelHeight) maxY = panelHeight - 1;
    for (int row = minY; row <= maxY; ++row) {
      const uint8_t* p = frameBuffer + static_cast<size_t>(row) * panelWidthBytes;
      const int from = minX >> 1;
      const int to = maxX >> 1;
      for (int i = from; i <= to; ++i) {
        const uint8_t v = p[i];
        if (v != 0x00 && v != 0xFF && v != 0x0F && v != 0xF0) return true;
      }
    }
  }
#endif
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return false;
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  // Measure with the same font drawText would render with (see resolveTextFontId)
  // so wrapping, truncation and centering of non-Latin strings stay consistent.
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  // Redirected to the SD fallback: batch-load the string's glyphs so the
  // per-codepoint measurement loop below doesn't fault them in one SD read
  // at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, renderedText, style, true);
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(renderedText, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir,
                           const int8_t tracking) const {
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  // Route non-Latin strings to the fallback font when the requested font
  // lacks the glyphs (e.g. Chinese book titles drawn with a Latin UI font).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  // Baseline from the resolved font; when the string was redirected to the
  // fallback, the caller positioned this line with the REQUESTED font's
  // metrics (row bands, icon centering), so center the fallback's line box
  // inside the requested font's line box instead of letting a taller/shorter
  // fallback hang below or float above the row's visual center.
  int yPos = y + getFontAscenderSize(resolvedFontId);
  if (resolvedFontId != fontId) {
    yPos += (getLineHeight(fontId) - getLineHeight(resolvedFontId)) / 2;
  }
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(renderedText, resolvedFontId, style);
    return;
  }

  // Redirected to the SD fallback: batch-load the string's glyphs so the draw
  // loop below doesn't fault them in one SD read at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, renderedText, style, false);
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }
  const auto& font = fontIt->second;

  const char* textCursor = renderedText;
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                       combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    const uint32_t smallCapCp = (style & EpdFontFamily::SMALL_CAPS) ? utf8SmallCapsUpper(cp) : cp;
    const bool isSmallCap = smallCapCp != cp;
    if (style & EpdFontFamily::SMALL_CAPS) {
      cp = smallCapCp;  // no ligatures: small caps set each letter separately
    } else {
      cp = font.applyLigatures(cp, textCursor, style);
    }

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP) + trackingBetween(prevCp, cp, tracking);
    }

    EpdGlyph solidFallback;
    const EpdGlyph* glyph = font.getGlyphMetrics(cp, solidFallback, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (isSupSub) {
      // Halve the advance so the cursor advances by the same amount the scaled glyph
      // actually occupies, keeping spacing correct without needing a separate smaller font.
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    } else if (isSmallCap) {
      prevAdvanceFP = scaleSmallCapAdvance(prevAdvanceFP);
    }

    if (isSupSub) {
      // yPos already carries the vertical offset applied by TextBlock::render().
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, style, 1, 2);
    } else if (isSmallCap) {
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, style, SMALL_CAPS_SCALE_NUM,
                       SMALL_CAPS_SCALE_DEN);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    // Byte-level scan: skip BiDi when no RTL script lead bytes are present.
    // Hebrew UTF-8 lead bytes: 0xD6-0xD7; Arabic/Syriac: 0xD8-0xDB.
    // This covers all RTL content without false negatives and avoids triggering
    // the full UAX#9 algorithm for Latin-extended, em-dashes, accented text, etc.
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  // Keep the border inside [x, x+width) like the thin overload: the previous
  // right/bottom edges at x+width / y+height sat one pixel outside the rect,
  // so stroked boxes looked shifted against fills computed from the rect.
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - 1 - i, y + i, state);
    drawLine(x + width - 1 - i, y + i, x + width - 1 - i, y + height - 1 - i, state);
    drawLine(x + width - 1 - i, y + height - 1 - i, x + i, y + height - 1 - i, state);
    drawLine(x + i, y + height - 1 - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    fillRect(left, py, width, 1, state);
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (state) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray>(x, y, width, height);
      break;
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max({0, x, clipLeft_});
  const int ly0 = std::max({0, y, clipTop_});
  const int lx1 = std::min({screenW, x + width, clipRight_});
  const int ly1 = std::min({screenH, y + height, clipBottom_});
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // The bounding rect in physical space is the rect we need to fill — rotation
  // is rigid (no shear/stretch) so the bbox of the two corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip Y range to the active band and redirect writes.
  uint8_t* target = getWriteTarget();
  // BATT5: a rect fill is a canvas mutation unless it went to the strip scratch.
  if (!_stripActive) noteFramebufferWrite();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  // Bit/byte layout: MSB-first within a byte, so phyX → bit (7 - (phyX & 7)).
  // Head and tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

#if CP_RENDER_4BPP
  if (!_stripActive) {
    // 4 bpp canvas: a rect fill is a nibble fill, so head/tail partial bytes are
    // two nibble splices and the middle is one memset of nib*0x11.
    //
    // ANTI-ALIASING IS FOR GLYPH EDGES ONLY. A LightGray/DarkGray fill is a
    // HALFTONE (a 2x2 pattern of ink and paper dots), never a solid optical
    // level: r99.9 drew it through drawPixelDither and the eye integrated the
    // dots; writing 0xA/0x5 instead turns every dithered fill, divider, disabled
    // border and table cell into a real grey surface, which (a) looks washed out
    // on glass and (b) puts the frame on the grey waveform for content that has
    // no coverage in it. Geometry stays exactly as crisp as it was: the same
    // 2x2 pattern, now written into the nibble plane.
    if constexpr (C == Color::LightGray || C == Color::DarkGray) {
      for (int ly = ly0; ly < ly1; ++ly) {
        for (int lx = lx0; lx < lx1; ++lx) {
          drawPixelDither<C>(lx, ly);
        }
      }
      return;
    } else {
      const uint8_t nib = C == Color::Black ? pixfmt::kInk : pixfmt::kPaper;
      const uint8_t fillByte = pixfmt::nibbleFillByte(nib);
      const int byteStart = phyX0 >> 1;
      const int byteEnd = phyX1 >> 1;  // inclusive
      // A nibble byte holds TWO pixels (low nibble = even phyX, high = odd), so
      // a partial byte at either end must fill EVERY nibble whose pixel lies in
      // [phyX0, phyX1] -- not just the single nibble nearest the rect's outer
      // edge. The head byte's INNER nibble and the tail byte's inner nibble are
      // not covered by the memset of the whole bytes between them, and masking
      // them out is what left 1 px paper columns INSIDE a fill: the 6 px band
      // rule at phyX 98..103 came out ink at 98,100,101,103 and paper at 99/102
      // (head byte's high nibble, tail byte's low nibble).
      for (int py = phyY0; py <= phyY1; ++py) {
        uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
        if (byteStart == byteEnd) {
          // One byte: fill exactly the nibbles whose pixels are inside the span.
          const uint8_t mask = static_cast<uint8_t>(((phyX0 & 1) ? 0x00u : 0x0Fu) | ((phyX1 & 1) ? 0xF0u : 0x00u));
          row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~mask) | (fillByte & mask));
        } else {
          // Head byte: partial only on its OUTER nibble -- its inner nibble is
          // inside the span. Tail byte: the mirror image.
          const uint8_t headMask = (phyX0 & 1) ? static_cast<uint8_t>(0xF0u) : static_cast<uint8_t>(0xFFu);
          const uint8_t tailMask = (phyX1 & 1) ? static_cast<uint8_t>(0xFFu) : static_cast<uint8_t>(0x0Fu);
          row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (fillByte & headMask));
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (fillByte & tailMask));
        }
      }
      return;
    }
  }
#endif

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer: 0 = black, 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Both patterns have period 2 in logical
    // (x, y), so per physical row we precompute one byte that represents the
    // pattern across an 8-pixel stretch — every full byte in the row uses
    // that same value.
    //
    // dlxPerPhyX / dlyPerPhyX: how logical (x, y) change as phyX increments
    // along a physical row. Derived from inverting rotateCoordinates.
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:
        dlxPerPhyX = 0;
        dlyPerPhyX = 1;
        break;
      case PortraitInverted:
        dlxPerPhyX = 0;
        dlyPerPhyX = -1;
        break;
      case LandscapeClockwise:
        dlxPerPhyX = -1;
        dlyPerPhyX = 0;
        break;
      case LandscapeCounterClockwise:
        dlxPerPhyX = 1;
        dlyPerPhyX = 0;
        break;
    }

    // The dither pattern has period 2 in logical space, and each orientation
    // maps py to logical coords with a fixed parity relationship. The
    // blackMask byte therefore repeats with period 2 in py. Precompute both
    // variants outside the row loop to eliminate the per-row switch + 8-bit
    // construction loop.
    uint8_t blackMasks[2];
    for (int parityIdx = 0; parityIdx < 2; ++parityIdx) {
      const int samplePy = phyY0 + parityIdx;
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - samplePy;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = samplePy;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - samplePy;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = samplePy;
          break;
      }
      uint8_t mask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) mask |= static_cast<uint8_t>(1u << (7 - b));
      }
      blackMasks[samplePy & 1] = mask;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      const uint8_t blackMask = blackMasks[py & 1];
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          // Period 2, so every full byte in this row is exactly whiteMask.
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  if constexpr (pixfmt::kFourBit) {
    // A 1 bpp asset (boot/sleep logos, moon icon — all small) lands as nibbles.
    // Per-pixel rather than FreeInkDisplay's byte blit, which writes 8 source
    // bits into one destination byte: in 4 bpp that byte holds two pixels, so
    // the blit would mangle every row.
    //
    // rotatedX/rotatedY are PHYSICAL panel coordinates — that is what the 1 bpp
    // path below hands blitImage(), and blitImage copies source (col,row) to
    // (rotatedX+col, rotatedY+row) with no further transform. So this branch
    // must write physically too: drawPixelLevel() takes LOGICAL coordinates and
    // rotates again, which point-reflected the image through the panel centre
    // and transposed it (boot logo off-centre, quick-resume moon pushed off the
    // panel entirely). The clip is consequently physical as well: commitPixelPhysical()
    // bounds-checks the panel and the active strip band in the same space it
    // writes, exactly like blitImage(), which also ignores the logical clip rect.
    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < width; ++col) {
        const uint8_t byte = bitmap[static_cast<size_t>(row) * ((width + 7) / 8) + (col >> 3)];
        // 1 bpp image convention (FreeInkDisplay::blitImage): 1 = white, 0 = ink.
        const bool white = ((byte >> (7 - (col & 7))) & 1) != 0;
        commitPixelPhysical(rotatedX + col, rotatedY + row, white ? pixfmt::kPaper : pixfmt::kInk);
      }
    }
    return;
  }
  // BATT5: the 1 bpp image blit writes canvas bytes through the display driver.
  noteFramebufferWrite();
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int size) const {
  // r163 D3: an icon is BINARY BY ROLE. Today this routine plots 1 bpp bits through
  // drawPixel(), so it already carries no level; the tag is what keeps that true if the
  // primitive is ever re-sourced (a 4 bpp sprite, a scaled blit) -- the admission check in
  // commitPixelPhysical and the glyph blits refuses intermediate levels for the role, not
  // for this one implementation.
  const ScopedContentRole roleTag(*this, ContentRole::Icon);
  // Plot the icon pixel-by-pixel through drawPixel (which applies the orientation
  // transform) instead of the byte-aligned framebuffer blit. The blit snaps the
  // icon's position to 8px (one byte) along the rotated axis, which prevents it
  // from aligning with adjacent text; per-pixel plotting is pixel-precise.
  // Icons are square and 1bpp (MSB-first, bit==0 = ink). The (size-1-row, col)
  // mapping reproduces the Portrait orientation the blit produced; drawIcon is
  // only called by the UI themes, which all render in forced Portrait.
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

bool GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY, const bool whiteAsTransparent) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return false;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    return drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
  }
#if CP_CPU_STATS
  // r96 IMAGE bucket, the 2-bit/halftone body. Started AFTER the early return
  // above so a 1-bit bitmap is counted once, in drawBitmap1Bit.
  const int64_t cpImageT0 = cpustats::nowUs();
#endif

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  // GRAY16: a 16-level bitmap draw is requested only by the sleep screen (see
  // setSixteenLevelImages) and only when the canvas can hold a nibble per pixel.
  // `bitmap.is1Bit()` is excluded because the 1-bit fast path above already
  // returned, and because a bilevel file has no tone to preserve.
  //
  // The gate is kGray16Sleep, not kGray16Images: the flag exists for the sleep
  // surface, so `-DCP_GRAY16_IMAGES=0` must not turn the sleep screen's bitmaps
  // back into 2-bit ink while the sleep surface itself is still enabled. Image
  // pages never set this flag (their 16 levels come from the in-book decoders).
  // r164 THUMB1: the second admitted surface. Home's cover thumbs are 2 bpp, and under
  // this flag their four palette indices are drawn as optical levels 0/5/10/15 instead
  // of being collapsed to ink by the `renderMode == BW` branch below. The flag is still
  // the caller's one-paint opt-in (drawCoverThumbFill sets it around a cover draw only);
  // this only says which SURFACES may set it. Separate from kGray16Sleep so either can
  // be rolled back alone.
  const bool gray16 = imageLevels16 && (pixfmt::kGray16Sleep || pixfmt::kGray16Covers) && pixfmt::kFourBit &&
                      renderMode == BW && !bitmap.is1Bit();

  // Calculate output row size (2 bits per pixel packed into bytes, or one
  // 16-level nibble per pixel for GRAY16).
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = gray16 ? (bitmap.getWidth() + 1) / 2 : (bitmap.getWidth() + 3) / 4;
  const auto rowBytesSize = static_cast<size_t>(bitmap.getRowBytes());
  auto rowScratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outputRowSize) + rowBytesSize);
  if (!rowScratch) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");

    return false;
  }

  auto* outputRow = rowScratch.get();
  auto* rowBytes = rowScratch.get() + outputRowSize;

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (gray16) {
      if (bitmap.readNextRow16(outputRow, rowBytes) != BmpReaderError::Ok) {
        LOG_ERR("GFX", "Failed to read 16-level row %d from bitmap", bmpY);
        return false;
      }
    } else if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);

      return false;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      uint8_t val = 0;
      if (!gray16) {
        val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
        if (whiteAsTransparent && val == 3) continue;
      }

      if (gray16) {
        // 16-level draw: EVEN x in the high nibble (Bitmap::readNextRow16). The
        // level goes straight to the canvas, where the 4 bpp nibble IS the optical
        // level the 16-level waveform will drive. Level 15 is paper, so an overlay
        // (whiteAsTransparent) skips it exactly as it skipped val==3 above.
        const uint8_t packed = outputRow[bmpX >> 1];
        const uint8_t level = (bmpX & 1) ? static_cast<uint8_t>(packed & 0x0F) : static_cast<uint8_t>(packed >> 4);
        if (whiteAsTransparent && level >= 15) continue;
        drawPixelLevel(screenX, screenY, level);
      } else if (renderMode == BW && val < 3) {
        // A bitmap's 2-bit value is a HALFTONE coordinate, not an optical level:
        // readNextRow produced it with an Atkinson/FS ditherer (or a native
        // palette quantiser), so the dots are meant to be ink and paper. Writing
        // the level through as 0x5/0xA turned every dithered picture -- the
        // custom sleep screen above all -- into a cloudy grey wash. Ink, as
        // before r100: the spatial pattern supplies the grey.
        drawPixel(screenX, screenY, true);
      } else if (renderMode == GRAYSCALE_LSB || renderMode == GRAYSCALE_MSB) {
        const auto pixel = grayPlanePixel(val, renderMode == GRAYSCALE_MSB, absoluteGrayPlanes);
        if (pixel.write) drawPixel(screenX, screenY, pixel.black);
      }
    }
  }

  const int sourceWidth = bitmap.getWidth() - cropPixX * 2;
  const int sourceHeight = bitmap.getHeight() - cropPixY * 2;
  const int renderedWidth = isScaled ? static_cast<int>(std::floor((sourceWidth - 1) * scale)) + 1 : sourceWidth;
  const int renderedHeight = isScaled ? static_cast<int>(std::floor((sourceHeight - 1) * scale)) + 1 : sourceHeight;
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
#if CP_CPU_STATS
  cpustats::noteImageUs(cpustats::nowUs() - cpImageT0);
#endif
  return true;
}

bool GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
#if CP_CPU_STATS
  // r96 `bench render` IMAGE bucket: the 1 bpp blit path an EPUB illustration
  // takes. One timer pair per image, not per pixel.
  const int64_t cpImageT0 = cpustats::nowUs();
#endif
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  const auto rowBytesSize = static_cast<size_t>(bitmap.getRowBytes());
  auto rowScratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outputRowSize) + rowBytesSize);
  if (!rowScratch) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");

    return false;
  }

  auto* outputRow = rowScratch.get();
  auto* rowBytes = rowScratch.get() + outputRowSize;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);

      return false;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    int screenY = y + (isScaled ? static_cast<int>(std::floor(bmpYOffset * scale)) : bmpYOffset);
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + (isScaled ? static_cast<int>(std::floor(bmpX * scale)) : bmpX);
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it). Ink, not a level: a 1-bit BMP is
      // already a halftone (see drawBitmap).
      if (val < 3) {
        drawPixel(screenX, screenY, true);
      }
      // White pixels (val == 3) are not drawn (leave background)
    }
  }

  const int renderedWidth =
      isScaled ? static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1 : bitmap.getWidth();
  const int renderedHeight =
      isScaled ? static_cast<int>(std::floor((bitmap.getHeight() - 1) * scale)) + 1 : bitmap.getHeight();
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
#if CP_CPU_STATS
  cpustats::noteImageUs(cpustats::nowUs() - cpImageT0);
#endif
  return true;
}

void GfxRenderer::preserveImagePolarity(const int x, const int y, const int width, const int height) const {
  if (renderMode != BW || !display.isInverted() || _stripActive || !frameBuffer || width <= 0 || height <= 0) {
    return;
  }

  const int lx0 = std::max(x, clipLeft_);
  const int ly0 = std::max(y, clipTop_);
  const int lx1 = std::min(x + width, clipRight_);
  const int ly1 = std::min(y + height, clipBottom_);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  int ax, ay, bx, by;
  rotateCoordinates(orientation, lx0, ly0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &bx, &by, panelWidth, panelHeight);

  int left = std::max(0, std::min(ax, bx));
  int right = std::min(static_cast<int>(panelWidth) - 1, std::max(ax, bx));
  int top = std::max(0, std::min(ay, by));
  int bottom = std::min(static_cast<int>(panelHeight) - 1, std::max(ay, by));
  if (left > right || top > bottom) return;
  // BATT5: the polarity fix-up rewrites canvas bytes in place.
  noteFramebufferWrite();

  for (int row = top; row <= bottom; row++) {
    uint8_t* rowData = frameBuffer + static_cast<uint32_t>(row) * panelWidthBytes;
#if CP_RENDER_4BPP
    if constexpr (pixfmt::kFourBit) {
      // 4 bpp: inverting a pixel is 0xF - level, per nibble. A byte-wide ~ would
      // be right only for whole bytes; the rect's edges are usually odd, so walk
      // pixels and splice.
      for (int col = left; col <= right; ++col) {
        uint8_t& cell = rowData[col >> 1];
        const uint8_t level = (col & 1) ? (cell >> 4) : (cell & 0x0F);
        const uint8_t inv = static_cast<uint8_t>(0x0F - level);
        cell = (col & 1) ? static_cast<uint8_t>((cell & 0x0F) | (inv << 4))
                         : static_cast<uint8_t>((cell & 0xF0) | inv);
      }
      continue;
    }
#endif
    int col = left;
    while (col <= right && (col & 7) != 0) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
    while (col + 7 <= right) {
      rowData[col >> 3] ^= 0xFF;
      col += 8;
    }
    while (col <= right) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto nodeX = makeUniqueNoThrow<int[]>(numPoints);
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX.get(), nodeX.get() + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  if (_stripActive) {
    // Clear only the active band's scratch, not the shared framebuffer. The
    // scratch is one 1 bpp controller PLANE per row (writeGrayscalePlaneStrip),
    // never the 4 bpp canvas, so its row width is the bit width regardless of
    // CP_RENDER_4BPP — using panelWidthBytes here overran the caller's buffer by
    // 4x the moment the canvas became 4 bpp.
    memset(_stripBuf, color, static_cast<size_t>(HalDisplay::DISPLAY_WIDTH / 8) * _stripRows);
    return;
  }
  // BATT5: the driver's clear writes every canvas byte.
  noteFramebufferWrite();
  display.clearScreen(color);
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}

void GfxRenderer::invertScreen() const {
  // BATT5: whole-canvas rewrite.
  noteFramebufferWrite();
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

namespace {
// r101.5: WHICH comparison the diff shadow supports.
//   0 = not decided yet (nothing allocated)
//   1 = EXACT: the shadow is the full frame (bufferSize bytes), compared byte for byte.
//   2 = SAMPLED: the full-size allocation failed, so the old 1-in-8 shadow is in use.
// The sampled grid lands on one 2-pixel column every 16 px (4 bpp, stride 608, 608 % 8 == 0), so
// inside the 21-px-wide status-bar battery glyph only pixels 560-561 were ever compared: a bolt
// appearing/disappearing read as "identical" and the paint returned BEFORE displayBuffer(), with
// the icon's own region still committing "painted". That is the bug this mode removes.
int g_diffShadowMode = 0;
bool g_diffShadowModeLogged = false;

void logDiffShadowMode(int mode, size_t bytes) {
  if (g_diffShadowModeLogged) return;
  g_diffShadowModeLogged = true;
  if (mode == 1) {
    LOG_INF("GFX", "redrive check: exact shadow ~%luKB (whole-frame compare, no sampling)",
            static_cast<unsigned long>(bytes / 1024));
  } else if (mode == 2) {
    LOG_INF("GFX", "redrive check: SAMPLED 1-in-8 (full shadow allocation failed)");
  } else {
    LOG_ERR("GFX", "redrive check: DISABLED (shadow allocation failed) -- every paint will drive");
  }
}
}  // namespace

// r101.5: ONE allocation site for the diff shadow, called by both users (changedRatioPercent(),
// framebufferSameAsShadow()). Tries the FULL-SIZE shadow first so the comparison can be exact;
// falls back to the previous bufferSize/8 sampled shadow only if PSRAM will not give us the
// frame-sized buffer, and says which one is in use exactly once. Returns false only when neither
// allocation succeeded (2's -1 "no baseline" contract).
bool GfxRenderer::ensureDiffShadow() const {
  if (prevSampleShadow_ != nullptr) return true;
  const size_t bytes = getBufferSize();
  if (bytes == 0) return false;
  uint8_t* exact = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  if (exact != nullptr) {
    prevSampleShadow_ = exact;
    g_diffShadowMode = 1;
    prevSampleValid_ = false;
    logDiffShadowMode(1, bytes);
    return true;
  }
  uint8_t* sampled = static_cast<uint8_t*>(heap_caps_malloc(bytes / 8 + 1, MALLOC_CAP_SPIRAM));
  if (sampled == nullptr) {
    logDiffShadowMode(0, bytes);
    return false;
  }
  prevSampleShadow_ = sampled;
  g_diffShadowMode = 2;
  prevSampleValid_ = false;
  logDiffShadowMode(2, bytes);
  return true;
}

int GfxRenderer::changedRatioPercent() const {
  const size_t bytes = getBufferSize();
  if (bytes == 0) return -1;
  if (!ensureDiffShadow()) return -1;
  const uint8_t* fb = getFrameBuffer();
  if (fb == nullptr) return -1;
  size_t total = 0;
  size_t changed = 0;
  if (g_diffShadowMode == 1) {
    // Exact: every byte is compared, so no feature of the frame can hide in an unsampled column.
    total = bytes;
    for (size_t i = 0; i < bytes; ++i) {
      const uint8_t v = fb[i];
      if (prevSampleValid_ && prevSampleShadow_[i] != v) ++changed;
      prevSampleShadow_[i] = v;
    }
  } else {
    for (size_t i = 0; i < bytes; i += 8, ++total) {
      const uint8_t v = fb[i];
      if (prevSampleValid_ && prevSampleShadow_[total] != v) ++changed;
      prevSampleShadow_[total] = v;
    }
  }
  const bool hadPrev = prevSampleValid_;
  prevSampleValid_ = true;
  if (!hadPrev || total == 0) return 100;  // first frame: treat as a full change
  return static_cast<int>((changed * 100) / total);
}

// r97.2 CP_SKIP_REDRIVE: a READ-ONLY reading of the shadow changedRatioPercent() maintains.
// changedRatioPercent() commits the baseline as a side effect, so it cannot be asked
// "did anything change?" without corrupting the next paint's diff -- this can.
//   1  the framebuffer is byte-identical to the frame the shadow last committed
//   0  it differs
//  -1  no baseline yet (nothing measured, or the shadow could not be allocated)
// r101.5: with the exact shadow this is a real memcmp over the whole frame, so `1` means the frame
// is GENUINELY identical -- not merely identical in one pixel column out of every sixteen.
int GfxRenderer::framebufferSameAsShadow() const {
  if (!prevSampleValid_ || prevSampleShadow_ == nullptr) return -1;
  const size_t bytes = getBufferSize();
  if (bytes == 0) return -1;
  const uint8_t* fb = getFrameBuffer();
  if (fb == nullptr) return -1;
  if (g_diffShadowMode == 1) return (memcmp(prevSampleShadow_, fb, bytes) == 0) ? 1 : 0;
  size_t sampled = 0;
  for (size_t i = 0; i < bytes; i += 8, ++sampled) {
    if (prevSampleShadow_[sampled] != fb[i]) return 0;
  }
  return 1;
}

HalDisplay::RefreshMode GfxRenderer::applyPromotedRefresh(const HalDisplay::RefreshMode refreshMode) const {
  if (autoRefreshPending_) {
    autoRefreshPending_ = false;
    const int ratio = changedRatioPercent();
    // Under 5% is a cursor move or a single row: DU handles it perfectly and costs ~0.3 s. Above
    // that the frame is being replaced, and DU would leave the old content on the glass.
    if (ratio >= 0 && ratio < autoRefreshThresholdPercent_) return HalDisplay::FAST_REFRESH;
    // AUTO must never escalate to a whole-panel full drive: a HALF (diff) refresh is the light
    // fallback here, and an explicit full refresh still arrives as a promoted mode below.
    return HalDisplay::HALF_REFRESH;
  }
  if (!promotedRefreshPending_) return refreshMode;
  promotedRefreshPending_ = false;
  return promotedRefresh_;
}

#if CP_RICKY_REFRESH_POLICY
// r94 (RICKY2 C4): the changed-pixel ratio THIS paint measured, or -1 when no AUTO was armed and
// nothing was measured. Written where the measurement already happens, because
// changedRatioPercent() also COMMITS the new baseline: recomputing the ratio for a log line would
// overwrite the shadow and corrupt the next paint's diff. The decision log reads this; nothing
// branches on it.
int g_paintDecisionRatio = -1;

// The request producer, verbatim from above, but returning the mode instead of
// driving it: ours measures (pipeline row 21 stays KEEP-OURS — the ratio is what
// decides), and only the ordering of AUTO-then-promotion is shared code.
HalDisplay::RefreshMode GfxRenderer::resolveRefreshRequest(const HalDisplay::RefreshMode refreshMode) const {
  int autoMode = CP_RM_HALF;
  bool autoPending = false;
  g_paintDecisionRatio = -1;
  if (autoRefreshPending_) {
    autoRefreshPending_ = false;
    autoPending = true;
    const int ratio = changedRatioPercent();
    g_paintDecisionRatio = ratio;
    autoMode = (ratio >= 0 && ratio < autoRefreshThresholdPercent_) ? CP_RM_FAST : CP_RM_HALF;
  }
  bool promotedPending = false;
  if (!autoPending && promotedRefreshPending_) {
    promotedRefreshPending_ = false;
    promotedPending = true;
  }
  return static_cast<HalDisplay::RefreshMode>(
      cp_refresh_resolve_request(static_cast<int>(refreshMode), autoPending ? 1 : 0, autoMode,
                                 promotedPending ? 1 : 0, static_cast<int>(promotedRefresh_)));
}
#endif

// The mode actually on the glass (pipeline row 7). File-scope rather than a
// member because the panel-park hook has no renderer reference and the app owns
// exactly one GfxRenderer; `g_lastAppliedValid` is what keeps "never painted" and
// "we no longer know" from being confused with "a diff was applied".
#if CP_RICKY_REFRESH_POLICY
namespace {
int g_lastAppliedMode = CP_LATCH_CLEAN;
bool g_lastAppliedValid = false;

// r93: the one-shot "Home is arriving from the reader, clean it" term. File-scope for the same
// reason as the latch above: the park hook has no renderer reference and needs to be able to
// disarm it, and the app owns exactly one GfxRenderer. Only ever set by
// ActivityManager::goHome() and only ever cleared by the next paint or a park.
bool g_forceCleanPaint = false;

// r97.2 CP_SKIP_REDRIVE: did the last COMPLETED drive clean the glass from white? Only then is
// "the frame in the framebuffer" also "the frame on the glass" for the whole panel, which is the
// precondition for skipping a repaint of the same frame. Cleared by any other drive (and by a
// failed one), so the guard can never fire over a frame the glass does not actually hold.
bool g_lastDriveFromWhiteClean = false;

// r96.2 (D1): how many paints have consumed the forced clean. The reader's image-page arm
// reads this before/after its drive, so an arm that was never consumed is caught at the call
// site instead of disappearing (which is exactly what r96.1 shipped: a clean that fired on
// neither edge and no evidence either way).
uint32_t g_cleanPaintsApplied = 0;
// r96.2 (D2): 1 = the glass was cleaned by the PREVIOUS paint, so the next differential paint
// needs no completion drive. Row 9 exists to finish a fast transition; it must not be spent on
// a frame that is already sitting on clean glass, which is what turned one wake drive into two.
int g_paintsSinceClean = 0;

// A drive's result exists only in the metalio panel shim. The reference is weak
// so every other board in the fork links with it absent (nullptr = no reporting)
// and keeps the r80 happy path; the Plus resolves it strongly.
extern "C" uint32_t metalio_vendor_panel_last_draw_error(void) __attribute__((weak));

uint32_t lastPanelDrawError() {
  if (metalio_vendor_panel_last_draw_error == nullptr) return 0;
  return metalio_vendor_panel_last_draw_error();
}

// r89: the panel shim's rail verdict for the drive that just ran. Implemented in src/main.cpp on top
// of the shim's permanent journal hook; weak so every other board links without it.
extern "C" uint32_t cp_panel_drive_seq(void) __attribute__((weak));
extern "C" bool cp_panel_drive_health(uint32_t since, int* tps, int* fast) __attribute__((weak));
// PG1: PWR_GOOD (TCA P07) sampled at the END of the drive's waveform, while the rails are still
// up. 1 = up, 0 = low, -1 = not reported. Independent of cp_panel_drive_health()'s power-up latch.
extern "C" int cp_panel_drive_pgood(uint32_t since) __attribute__((weak));

// Not an EpdDrawError: marks a drive the shim called successful but whose rails were down.
constexpr uint32_t kDriveHadNoRails = 0x80000000u;
}  // namespace

// Panel parked, deinitialised or powered off: the glass can no longer be inferred
// from the last mode we applied, so drop back to the clean term (pipeline rows 5
// and 7 — their "zeroed on deinit", 0x401905ca).
extern "C" void cp_refresh_policy_note_park(void) {
  g_lastAppliedMode = CP_LATCH_CLEAN;
  g_lastAppliedValid = false;
  // r93: a parked panel ends the transition the flag was armed for. Home is painted by the next
  // wake, and an unrelated from-white FULL there is exactly the leak this clears.
  g_forceCleanPaint = false;
}

void GfxRenderer::forceCleanNextRefresh() const { g_forceCleanPaint = true; }

// r96.2: observable from the call site (see GfxRenderer.h lastPaintForcedClean()).
extern "C" uint32_t cp_refresh_policy_clean_paints(void) { return g_cleanPaintsApplied; }

uint32_t GfxRenderer::forcedCleanPaintsConsumed() const { return g_cleanPaintsApplied; }

// r97 MAP1: read-only, no side effects -- the arm is consumed by displayBuffer(), never here.
bool GfxRenderer::forcedCleanArmed() const { return g_forceCleanPaint; }
#else
// r80: no latch to drop, and no clean-on-arrival term to arm.
extern "C" void cp_refresh_policy_note_park(void) {}

void GfxRenderer::forceCleanNextRefresh() const {}
extern "C" uint32_t cp_refresh_policy_clean_paints(void) { return 0; }
uint32_t GfxRenderer::forcedCleanPaintsConsumed() const { return 0; }
bool GfxRenderer::forcedCleanArmed() const { return false; }
#endif

// ---------------------------------------------------------------------------
// r101.9 WAKE3 — PAINT ORIGIN (diagnostics only). See the block in GfxRenderer.h.
// State: one counter and one borrowed pointer. Read by main.cpp's panel journal
// hook, written on the task that is painting. No decision here, so no drive can
// appear, vanish, change mode or change duration because of this block.
namespace {
uint32_t g_paintSeq = 0;             // displayBuffer()/displayBufferAsync() entries, never reset
const char* g_paintOwner = nullptr;  // activity name while a render-task paint composes
char g_paintOrigin[48] = "p0/M";     // last composed text, returned by cp_paint_origin_text()
}  // namespace

extern "C" void cp_paint_owner_note(const char* activity) { g_paintOwner = activity; }

extern "C" void cp_paint_owner_clear(void) { g_paintOwner = nullptr; }

// The paint entry currently running, as one token. `M` = not inside a render-task
// paint (onEnter/first-render, a follow-up request, the shell, the power-off path).
extern "C" const char* cp_paint_origin_text(void) {
  if (g_paintOwner != nullptr) {
    snprintf(g_paintOrigin, sizeof(g_paintOrigin), "p%lu/R:%s", static_cast<unsigned long>(g_paintSeq), g_paintOwner);
  } else {
    snprintf(g_paintOrigin, sizeof(g_paintOrigin), "p%lu/M", static_cast<unsigned long>(g_paintSeq));
  }
  return g_paintOrigin;
}

extern "C" uint32_t cp_paint_seq(void) { return g_paintSeq; }

// FLP2 — the vendor shim's "this paint is binary" arm.
//
// The reference is weak, exactly like `metalio_vendor_panel_last_draw_error`
// above: every board in this fork that does not build the metalio driver links
// with it absent (nullptr = no hook), and the Plus resolves it strongly.
// metalio_vendor_panel_binary_du_frames(1) means "the NEXT show() is binary
// content": no 8-optical-level grey table (metalio_vendor_panel.cpp:644, :728)
// and no from-white re-condition clean from the 24-partial cadence (:719-726).
// One frame only, decremented inside show() and again by (0) at park/init, so a
// device that never paints a frontlight adjustment executes the pre-FLP2 path
// byte for byte.
namespace {
extern "C" void metalio_vendor_panel_binary_du_frames(int frames) __attribute__((weak));

void cpPanelArmBinaryDu() {
  if (metalio_vendor_panel_binary_du_frames != nullptr) metalio_vendor_panel_binary_du_frames(1);
}
}  // namespace

// r97.2 CP_ENTRY_SINGLE_DRIVE: the entry gate. See PlatformFeatures.h for the mechanism; the
// short version is that a screen's first paint can otherwise land while onEnter() is still
// building its content, which costs a second whole-panel drive of the same screen.
void GfxRenderer::holdEntryDrive(const uint32_t timeoutMs) const {
  entryGateArmed_ = true;
  entryGateDeadlineMs_ = millis() + timeoutMs;
  entryGateHeldPaint_ = false;
}

bool GfxRenderer::releaseEntryDrive() const {
  const bool held = entryGateHeldPaint_;
  entryGateArmed_ = false;
  entryGateHeldPaint_ = false;
  return held;
}

bool GfxRenderer::entryGateArmed() const { return entryGateArmed_; }

bool GfxRenderer::lastPaintAttemptHeld() const { return entryGateHeldLast_; }

// r161 MENU AA: last line of defence for a paint declared BINARY ("Menu AA" off).
//
// The glyph choke point (commitPixelPhysical) already stops coverage levels at
// the source, which covers every glyph and every explicit-level writer. What it
// cannot cover is a bulk copy into the canvas — a cached cover/thumbnail blitted
// as a 4 bpp region, `writeFramebufferRegion`, a restored buffer. One such byte
// is enough to put the whole frame back on the 8-level grey wipe
// (`frameHasGrey`), which is exactly what the setting exists to avoid. So a
// binary paint also gets one pass over the composed frame, and the four byte
// patterns a purely binary frame can produce (0x00/0xFF/0x0F/0xF0) are the only
// ones that survive it.
//
// Only ever called when textBinary_ is set, so an AA paint (every build before
// r161) pays not one instruction here.
static void binarizeFrameIfNeeded(uint8_t* buffer, size_t bytes) {
#if CP_RENDER_4BPP
  if (!buffer) return;
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t v = buffer[i];
    if (v == 0x00 || v == 0xFF || v == 0x0F || v == 0xF0) continue;
    uint8_t out = 0;
    for (int shift = 0; shift <= 4; shift += 4) {
      const uint8_t nib = static_cast<uint8_t>((v >> shift) & 0x0F);
      out = static_cast<uint8_t>(out | ((nib < 8 ? pixfmt::kInk : pixfmt::kPaper) << shift));
    }
    buffer[i] = out;
  }
#else
  (void)buffer;
  (void)bytes;
#endif
}

void GfxRenderer::displayBuffer(HalDisplay::RefreshMode refreshMode) const {
  if (textBinary_) binarizeFrameIfNeeded(frameBuffer, frameBufferSize);
  // r101.9 WAKE3: one tick per PAINT ENTRY, before the entry gate can return,
  // so a held paint still gets its own number (it drives nothing, so it prints
  // no drive line) and two drive lines sharing a number came from one entry.
  ++g_paintSeq;
  entryGateHeldLast_ = false;
#if CP_ENTRY_SINGLE_DRIVE
  // The gate is checked BEFORE the producer runs: a held paint must not consume the armed
  // promotion, the forced clean, the AUTO measurement or the diff baseline, or the drive it
  // defers would arrive with different terms than the one it replaced.
  if (entryGateArmed_) {
    if (static_cast<int32_t>(millis() - entryGateDeadlineMs_) < 0) {
      entryGateHeldPaint_ = true;
      entryGateHeldLast_ = true;
      lastPaintForcedClean_ = false;
      LOG_INF("GFX", "entry gate: first drive held -- content not built yet (armed for %d ms, %lu ms spent)",
              CP_ENTRY_GATE_TIMEOUT_MS, static_cast<unsigned long>(millis() - (entryGateDeadlineMs_ - CP_ENTRY_GATE_TIMEOUT_MS)));
      return;
    }
    // Budget spent: the screen is driven as-is. A missing cover or a slow card costs one stale
    // frame, never a held or blank screen.
    entryGateArmed_ = false;
    LOG_INF("GFX", "entry gate: %d ms budget expired -- driving the frame as built", CP_ENTRY_GATE_TIMEOUT_MS);
  }
#endif
#if CP_CPU_STATS
  // r96 `bench render` DRIVE bucket: measured around the EXISTING
  // display.displayBuffer() call boundary, never inside epdiy.
  const int64_t cpDriveT0 = cpustats::nowUs();
#endif
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  // FLP1: consume the one-shot BEFORE the drive decision, so `no completion` is
  // a property of THIS paint and cannot leak into the next one. Deliberately
  // below the entry gate: a paint the gate held drives nothing, so the arm stays
  // armed for the payback paint that actually reaches the glass (the gate held
  // nothing of this paint's terms either -- see the gate's own comment).
  [[maybe_unused]] const bool completionSuppressed = suppressCompletionOnce_;
  suppressCompletionOnce_ = false;
  // FLP2: consume the forced-binary one-shot on the same terms as the FLP1
  // suppression above -- below the entry gate, so a paint the gate held leaves
  // the arm for the payback paint that actually reaches the glass.
  const bool binaryDuForced = binaryDuOnce_;
  binaryDuOnce_ = false;
  const HalDisplay::RefreshMode requested = refreshMode;
#if !CP_RICKY_REFRESH_POLICY
  refreshMode = applyPromotedRefresh(refreshMode);
  // FLP2: the panel's forced-binary paint is a pin on the drive mode as well as
  // on the shim's waveform choice (see forceBinaryDuOnce).
  if (binaryDuForced) refreshMode = HalDisplay::FAST_REFRESH;
  // r96.2: this build has no forced-clean term at all, so no paint consumes one.
  lastPaintForcedClean_ = false;
  // Which drive actually reaches the glass, per paint: the requested mode and the one the promotion
  // chain resolved it to. One line per paint is what makes "two flashes" countable in the log.
  LOG_DBG("GFX", "paint req=%d applied=%d", static_cast<int>(requested), static_cast<int>(refreshMode));
  if (binaryDuForced) cpPanelArmBinaryDu();
  display.displayBuffer(refreshMode, fadingFix);
  // BATT5: this build has no drive verdict here (no error code, no rail check), so the
  // glass epoch is deliberately NOT re-based: CP_ICON_SAFE_PATCH fails CLOSED and every
  // plug/unplug takes the whole-activity path. The shipped build compiles the policy
  // branch below, which does have the verdict and does re-base it.
#if CP_CPU_STATS
  cpustats::noteDriveUs(cpustats::nowUs() - cpDriveT0, static_cast<uint8_t>(refreshMode));
#endif
#else
  // Producer (ours: measured ratio + armed promotion), then policy (theirs:
  // clean precedence, no-HALF-after-FULL). Rows 4/5/8/21 of the pipeline table.
  // r94 (C4): the producer CLEARS the armed bits and commits the diff baseline, so every decision
  // input is snapshotted here, before the call, and logged after the policy has run. Reads only --
  // nothing below branches on these locals.
  [[maybe_unused]] const bool shadowHadPrev = prevSampleValid_;
  [[maybe_unused]] const bool autoArmed = autoRefreshPending_;
  [[maybe_unused]] const bool promoArmed = promotedRefreshPending_;
  [[maybe_unused]] const int promoMode = static_cast<int>(promotedRefresh_);
  // r101.5 ORDERING: read the identity verdict BEFORE the producer runs. resolveRefreshRequest()
  // -> changedRatioPercent() COMMITS the current frame into the shadow as a side effect, so a
  // comparison taken after it would be against the frame we just wrote down -- "identical" on every
  // AUTO paint, i.e. the wrong skip, now hidden behind an exact compare. Only paid for when the
  // skip guard could actually fire.
  [[maybe_unused]] const int sameAsShadowAtEntry =
      g_lastDriveFromWhiteClean ? framebufferSameAsShadow() : -1;
  refreshMode = resolveRefreshRequest(refreshMode);
  [[maybe_unused]] const int producerMode = static_cast<int>(refreshMode);
  // r93: Home arriving from the reader. The flag is a TERM of the policy below, not a branch
  // around it, and it is consumed HERE -- before the drive, not after a successful one. A paint
  // that is skipped, aborts, or fails therefore cannot leave it armed for the next Home paint:
  // the transition buys at most one 815 ms from-white GC16 for the whole session.
  // r97.1 WAKE1 (diagnostics only, no decision change): the wake arms ONE forced clean
  // (main.cpp, "[WAKE] arm"). It is a file-scope one-shot, so the first displayBuffer() after
  // the arm takes it and every later paint in the same wake window sees force=0. This line
  // makes that visible per paint -- `[GFX] arm armed=1` is the paint that IS the wake clean,
  // `armed=0 latch=1` is a later differential paint, i.e. a second drive inside the wake.
  LOG_DBG("GFX", "arm req=%d armed=%d promo=%d valid=%d latch=%d (only armed=1 takes the wake clean)",
          static_cast<int>(requested), g_forceCleanPaint ? 1 : 0, promotedRefreshPending_ ? 1 : 0,
          g_lastAppliedValid ? 1 : 0, g_lastAppliedMode);
  const bool forceClean = g_forceCleanPaint;
  g_forceCleanPaint = false;
  lastPaintForcedClean_ = forceClean;
  if (forceClean) {
    ++g_cleanPaintsApplied;
    g_paintsSinceClean = 1;
    // r96.2 (D1) CONSUME SITE. This line plus the reader's `[ERS] armImageClean`
    // line is the handoff: if the arm is logged and this is not, the paint that
    // followed the arm did not take the flag and the arm is re-committed (see
    // EpubReaderActivity::renderContents).
    LOG_INF("GFX", "forced clean consumed by this paint (cleanPaints=%lu)",
            static_cast<unsigned long>(g_cleanPaintsApplied));
    // r97.1 WAKE1: the arm is spent HERE. Any paint after this one during the same wake is
    // unarmed by construction, which is what makes a second drive in the wake a caller-side
    // problem (an async repaint), never a missing arm.
    LOG_INF("GFX", "arm: consumed by this paint -- later paints in this wake see force=0 (latch=%d)",
            g_lastAppliedMode);
  }
#if CP_WAKE_ONE_DRIVE
  // r96.2 (D2): the previous paint cleaned the glass from white, so this differential
  // paint over the SAME content has nothing to finish -- spending row 9 on it is the
  // second full-screen waveform the operator reads as banding after a wake.
  //
  // r101.13b (B-P1-5): "the SAME content" used to be assumed from the paint counter alone, so the
  // first FAST transition after any wake -- usually over DIFFERENT content -- also lost its
  // completion drive, which is the banding the log claimed it was avoiding. The identity predicate
  // already existed: sameAsShadowAtEntry == 1 is the whole-frame memcmp of this paint against the
  // frame the from-white clean drove (read before the producer commits the shadow, see r101.5).
  // The cost of being wrong on that compare is one extra GL16 on an unchanged frame; the cost of the
  // old counter was an unfinished transition, so the identity term wins.
  [[maybe_unused]] const bool skipCompletion =
      (!forceClean && g_paintsSinceClean > 0 && sameAsShadowAtEntry == 1);
#if CP_WAKE_ONE_DRIVE_STRICT
  // r114b (E2): the first paint after a forced clean takes NO completion drive even when the
  // identity compare says the pixels moved (covers/text arriving after the clean). Read BEFORE
  // the counter is cleared below; it is true for exactly one paint.
  const bool postCleanPaint = (!forceClean && g_paintsSinceClean > 0);
#else
  [[maybe_unused]] const bool postCleanPaint = false;
#endif
  if (!forceClean && g_paintsSinceClean > 0) g_paintsSinceClean = 0;
#else
  [[maybe_unused]] const bool skipCompletion = false;
  [[maybe_unused]] const bool postCleanPaint = false;
#endif
  const bool clean = (!g_lastAppliedValid || g_lastAppliedMode == CP_LATCH_CLEAN);
  const int previous = g_lastAppliedValid ? g_lastAppliedMode : CP_LATCH_CLEAN;
  // A whole-frame replacement: the caller asked for the from-white drive, the producer resolved
  // to it, or this paint is the one-shot clean-on-arrival above.
  const bool fullRequest = forceClean || (requested == HalDisplay::FULL_REFRESH || refreshMode == HalDisplay::FULL_REFRESH);
  if (forceClean) refreshMode = HalDisplay::FULL_REFRESH;
  refreshMode = static_cast<HalDisplay::RefreshMode>(
      cp_refresh_resolve_mode(static_cast<int>(refreshMode), clean ? 1 : 0, previous, fullRequest ? 1 : 0));
  // FLP2: the panel's forced-binary adjust paint is the LAST word on the mode.
  // Applied after the policy stage on purpose: the two terms that can still
  // escalate a FAST request live here -- rule 3 (`previous == CP_LATCH_FULL` ->
  // HALF, e.g. the reader underneath did a from-white page clean) and the
  // shim's cadence (which reads a mode, not this flag). A promotion cannot
  // express this, because the producer lets an armed AUTO outrank it.
  if (binaryDuForced) refreshMode = HalDisplay::FAST_REFRESH;

  const uint32_t healthSeq = (cp_panel_drive_seq != nullptr) ? cp_panel_drive_seq() : 0;
#if CP_SKIP_REDRIVE
  // r97.2: a paint that carries the SAME frame a from-white clean just drove has nothing to say
  // to the glass. The test is the diff shadow, read without committing it, so a skipped paint
  // cannot corrupt the next diff (and read BEFORE the producer commits it, see sameAsShadowAtEntry).
  // r101.5: with the full-size shadow that test is a whole-frame memcmp, so the icon column the
  // 1-in-8 grid stepped over can no longer make a changed frame look identical.
  // Never skips: a paint that consumes the forced clean, a paint that asked for a whole-panel mode,
  // or one with no diff baseline yet.
  if (!forceClean && !fullRequest && g_lastDriveFromWhiteClean && sameAsShadowAtEntry == 1) {
    LOG_INF("GFX", "redrive skipped: framebuffer is byte-identical to the frame the last from-white clean drove");
    g_lastDriveFromWhiteClean = false;
    // BATT5: no drive happened, but the reason is a whole-frame memcmp that says this
    // canvas IS the frame the last clean drove -- the strongest evidence available, so
    // the glass epoch may be re-based on it. Without this a skipped redrive would leave
    // the epochs unequal over a canvas that is genuinely on the glass.
    glassEpochs_.noteDriveLanded();
    return;
  }
#endif
  const uint32_t driveStart = millis();
  // r94 (C4): the DECISION, not just its result. One DEBUG line per paint carrying every input the
  // two policy stages consumed, so a ghost / double flash / wrong mode can be attributed to a branch
  // from the log alone. Stable field order, one token per field:
  //   req    mode the call site asked for
  //   res    mode the producer resolved to (AUTO ratio / armed promotion), before the policy stage
  //   force  1 = the r93 one-shot clean-on-arrival term was set for this paint
  //   ratio  changed-pixel percent measured for THIS paint (-1 = not measured: no AUTO was armed)
  //   shadow 1 = a previous-frame diff baseline existed when the paint started
  //   auto   1 = an AUTO was armed at entry; promo 1 = an explicit promotion was armed;
  //   mode   what that promotion asked for; valid 1 = the latch describes what is on the glass
  //   clean  1 = the panel was known-clean (never painted since boot/park)
  //   prev   the latch value handed to the policy
  //   end    the mode actually resolved for the drive (the policy's output)
  LOG_DBG("GFX",
          "paintdec req=%d res=%d force=%d ratio=%d shadow=%d auto=%d promo=%d mode=%d valid=%d clean=%d prev=%d end=%d bin=%d",
          static_cast<int>(requested), producerMode, forceClean ? 1 : 0, g_paintDecisionRatio,
          shadowHadPrev ? 1 : 0, autoArmed ? 1 : 0, promoArmed ? 1 : 0, promoMode, g_lastAppliedValid ? 1 : 0,
          clean ? 1 : 0, previous, static_cast<int>(refreshMode), binaryDuForced ? 1 : 0);

  if (binaryDuForced) cpPanelArmBinaryDu();
  display.displayBuffer(refreshMode, fadingFix);
#if CP_CPU_STATS
  cpustats::noteDriveUs(cpustats::nowUs() - cpDriveT0, static_cast<uint8_t>(refreshMode));
#endif
  uint32_t result = lastPanelDrawError();

  // r89: the shim's error code is not sufficient evidence. show() drives even when the TPS65185
  // power-up did not come back (metalio_vendor_panel.cpp:372-381 falls through with g_powered=false)
  // or when the PWR_GOOD read could not be taken and the code assumed "up" (:358-365); epdiy then
  // reports EPD_DRAW_SUCCESS for a waveform clocked with no rail under it. The frame never landed,
  // but epdiy commits back_fb = front_fb anyway (highlevel.c:262-282), so from then on every diff
  // targets content the glass does not have -- permanent ghosting, and exactly the artifact this
  // release is chasing. The rail verdict is the ground truth, so it outranks the error code.
  //
  // PG1: THE EVIDENCE SOURCE. `tps` is the power-up LATCH (epd_board_*_tps_ok()). On this board the
  // rails stay up across drives, so a rail that collapses DURING a waveform leaves the latch at 1
  // and this check could not fire -- a drive clocked with no rail under it was recorded as a
  // SUCCESS. The TPS PG register is not a substitute for the same job: outside the power-up window
  // it reads 0x00 with the rails up (live `panel`: pg_reg=0x00, rails_up=0x00, pwr_good=1, dpr
  // fine), so keying off it is either a detector that never fires or one that fires on every paint.
  // The second source is therefore PWR_GOOD (TCA P07), sampled by the shim at the END of the
  // waveform, while the rails are still supposed to be up -- the one window where the pin is
  // meaningful (the TPS drives it open-drain and releases it when parked). Rules:
  //   * the latch still decides first, exactly as before: tps==0 -> did not land;
  //   * the pin may only ADD a failure, and only on a definite low: pgood==0 -> did not land;
  //   * pgood==-1 (not reported / expander read failed) is unknown and changes nothing.
  // No rule here can turn a healthy drive into a failure, which is what keeps this from becoming a
  // machine that forces a from-white FULL repaint on every paint.
  int railsTps = -1;
  int railsPgood = -1;
  if (result == 0 && cp_panel_drive_health != nullptr &&
      cp_panel_drive_health(healthSeq, &railsTps, nullptr)) {
    if (railsTps == 0) {
      result = kDriveHadNoRails;
      LOG_ERR("GFX", "paint mode=%d reported success but the panel rails were DOWN (tps=0): frame did not land",
              static_cast<int>(refreshMode));
    } else if (cp_panel_drive_pgood != nullptr) {
      railsPgood = cp_panel_drive_pgood(healthSeq);
      if (railsPgood == 0) {
        result = kDriveHadNoRails;
        LOG_ERR("GFX",
                "paint mode=%d reported success but PWR_GOOD was LOW at the end of the waveform (tps=1): "
                "rail evidence disagrees, frame did not land",
                static_cast<int>(refreshMode));
      }
    }
  }

  // Row 7: the latch is written ONLY on a successful drive. Row 12: a failed
  // drive leaves the shadow describing content the glass never got, so declare it
  // stale (drop the latch, drop the diff baseline) and make the next paint a
  // from-white FULL — a retry that diffs against the stale front buffer is exactly
  // what row 12 exists to prevent.
  if (result == 0) {
    // Collapsed encoding: a genuine from-white FULL is what the row-8 term tracks;
    // anything cheaper (diff GC16, DU, GL16) is "fast" for that purpose. Writing
    // CP_LATCH_FULL after a row-8 promotion or after a completion drive would make
    // the promotion repeat forever — see RefreshPolicy.h.
    g_lastAppliedMode = (requested == HalDisplay::FULL_REFRESH) ? CP_LATCH_FULL : CP_LATCH_FAST;
    g_lastAppliedValid = true;
    // BATT5: the ONE place a landed drive is recorded. Same verdict as the latch above
    // (error 0 AND the rails did not disagree), i.e. the driver reached the memcpy of
    // back_fb = front_fb at highlevel.c:136 -- from here the canvas and the glass hold
    // the same bytes, which is what licences a region patch (GlassEpoch.h).
    glassEpochs_.noteDriveLanded();
#if CP_SKIP_REDRIVE
    // r97.2: the frame in the framebuffer is now also the frame on the glass for the WHOLE panel,
    // which is what lets the next paint of an identical frame be skipped instead of re-driven.
    g_lastDriveFromWhiteClean = forceClean && refreshMode == HalDisplay::FULL_REFRESH;
#endif
  } else {
    g_lastAppliedMode = CP_LATCH_CLEAN;
    g_lastAppliedValid = false;
    // BATT5: fail CLOSED. The panel may be half-driven and this canvas is known not to
    // be on the glass; advancing the compose epoch makes the two counters unequal even
    // when no pixel was touched, so no region patch can be licensed against it.
    glassEpochs_.noteDriveFailed();
#if CP_SKIP_REDRIVE
    // The glass did not take the frame: nothing may be skipped against it.
    g_lastDriveFromWhiteClean = false;
#endif
    prevSampleValid_ = false;
    promotedRefresh_ = HalDisplay::FULL_REFRESH;
    promotedRefreshPending_ = true;
    autoRefreshPending_ = false;
    LOG_ERR("GFX", "paint drive failed err=0x%x: shadow declared stale, next paint from-white",
            static_cast<unsigned>(result));
  }

#if CP_RICKY_COMPLETION_DRIVE
  // Row 9: a fast transition is finished by a full drive. GL16, because nothing is
  // known about a follow-up paint here; the latch deliberately stays "fast" so the
  // completion is part of this transition and does not arm row 8 against the next one.
  // EDRV1 (r101.21): the arm is consulted INSIDE the existing compile-time block.
  // At the shipped default it is true, so the drive happens exactly as in r101.20;
  // `exp row9 off` suppresses it for this session and nothing else.
  if (result == 0 && refreshMode == HalDisplay::FAST_REFRESH && exparms::completionDrive() &&
      !completionSuppressed) {
    if (skipCompletion) {
      // r96.2 (D2): the previous paint was a from-white clean of the same content, so
      // the completion has nothing left to clean. Driving it anyway is a second
      // whole-panel waveform on an unchanged frame -- the wake "band".
      LOG_INF("GFX", "row9 completion skipped: previous paint was a from-white clean over the same content");
    } else if (postCleanPaint) {
      // r114b (E2, CP_WAKE_ONE_DRIVE_STRICT): same rule, but decided by the PAINT COUNTER
      // instead of the pixel compare. The glass was cleaned one paint ago; a completion here
      // is the second whole-panel drive the operator reads as a band. Nothing else in the
      // paint changes -- same mode, same duration, no arm, no timer.
      LOG_INF("GFX", "row9 completion skipped (strict): first paint after a from-white clean, mode=%d",
              static_cast<int>(refreshMode));
    } else {
      // r97 MAP1: the completion is a REAL second whole-panel drive and it used to be
      // invisible in the log (it calls display.displayBuffer() directly, below the paint
      // line). One line here makes "how many drives did this transition perform" a count
      // from the log instead of an inference.
      const uint32_t completionStart = millis();
      display.displayBuffer(HalDisplay::GL16_REFRESH, fadingFix);
      LOG_INF("GFX", "row9 completion drive appended (mode=%d ms=%lu): second whole-panel drive for this transition",
              static_cast<int>(HalDisplay::GL16_REFRESH), static_cast<unsigned long>(millis() - completionStart));
      result = lastPanelDrawError();
      if (result != 0) {
        g_lastAppliedMode = CP_LATCH_CLEAN;
        g_lastAppliedValid = false;
        // BATT5: the completion drive is a SECOND whole-panel waveform over this canvas.
        // If it did not land the panel is left in an unknown state, so the canvas must
        // stop claiming to be the glass even though the first drive had landed.
        glassEpochs_.noteDriveFailed();
      }
    }
  }
#endif

  // Row 23: one line per paint carrying the RESOLVED mode and the terms that
  // produced it, plus the wall time of the drive(s). Ours logged intent; this is
  // what makes a ghosting report diagnosable.
  // r89 adds `rails` (the TPS verdict for this drive; -1 = not reported) and `landed`
  // (0 = the frame demonstrably did not reach the glass), because a paint can be
  // "successful" and still never have been driven.
  // PG1 adds `pg` (PWR_GOOD at the end of the waveform; -1 = not reported) so the two rail
  // sources are comparable in one line: rails=1 pg=0 is the disagreement the detector now fires on.
  LOG_DBG("GFX", "paint req=%d applied=%d full=%d clean=%d previous=%d force=%d result=0x%x rails=%d pg=%d landed=%d ms=%lu",
          static_cast<int>(requested), static_cast<int>(refreshMode), fullRequest ? 1 : 0, clean ? 1 : 0,
          previous, forceClean ? 1 : 0, static_cast<unsigned>(result), railsTps, railsPgood,
          (result == kDriveHadNoRails) ? 0 : 1, static_cast<unsigned long>(millis() - driveStart));
#endif
}

void GfxRenderer::displayBufferAsync(HalDisplay::RefreshMode refreshMode) const {
  if (textBinary_) binarizeFrameIfNeeded(frameBuffer, frameBufferSize);
  // r101.9 WAKE3: same paint-entry tick as the blocking path (see displayBuffer()).
  ++g_paintSeq;
  // FLP1: this path never runs the row-9 completion drive at all, so the one-shot
  // is spent here rather than left armed for a later, unrelated paint.
  suppressCompletionOnce_ = false;
  // FLP2: same contract for the forced-binary one-shot -- spent here, and spent
  // BEFORE the drive so it can never leak into a later paint.
  const bool binaryDuForcedAsync = binaryDuOnce_;
  binaryDuOnce_ = false;
#if CP_RICKY_REFRESH_POLICY
  // Same producer + policy as the blocking path, but no latch write: the drive is
  // deferred, so there is no result to record and inventing one would break row 7's
  // "written only on a successful drive". The latch therefore still describes the
  // last BLOCKING paint, which is the r80-conservative reading.
  // r94 (C4): snapshot the decision inputs before the producer clears them, then log the same field
  // set as the blocking path after the policy stage -- one `paintdec` line per paint on both paths.
  // No ms on this path: the drive is deferred, so there is no measured wall time to report.
  [[maybe_unused]] const bool shadowHadPrevAsync = prevSampleValid_;
  [[maybe_unused]] const bool autoArmedAsync = autoRefreshPending_;
  [[maybe_unused]] const bool promoArmedAsync = promotedRefreshPending_;
  [[maybe_unused]] const int promoModeAsync = static_cast<int>(promotedRefresh_);
  refreshMode = resolveRefreshRequest(refreshMode);
  [[maybe_unused]] const int producerModeAsync = static_cast<int>(refreshMode);
  // r93: same one-shot clean-on-arrival, consumed here too so a deferred paint can never be the
  // place the flag silently survives. No latch write on this path (see below).
  const bool forceClean = g_forceCleanPaint;
  g_forceCleanPaint = false;
  lastPaintForcedClean_ = forceClean;
  if (forceClean) ++g_cleanPaintsApplied;
  const bool clean = (!g_lastAppliedValid || g_lastAppliedMode == CP_LATCH_CLEAN);
  if (forceClean) refreshMode = HalDisplay::FULL_REFRESH;
  const HalDisplay::RefreshMode requestedAsync = refreshMode;
  refreshMode = static_cast<HalDisplay::RefreshMode>(cp_refresh_resolve_mode(
      static_cast<int>(refreshMode), clean ? 1 : 0, g_lastAppliedValid ? g_lastAppliedMode : CP_LATCH_CLEAN,
      (refreshMode == HalDisplay::FULL_REFRESH) ? 1 : 0));
  // FLP2: same last-word pin as the blocking path (see its comment).
  if (binaryDuForcedAsync) refreshMode = HalDisplay::FAST_REFRESH;
  LOG_DBG("GFX", "paint async req=%d applied=%d clean=%d previous=%d force=%d", static_cast<int>(requestedAsync),
          static_cast<int>(refreshMode), clean ? 1 : 0, g_lastAppliedValid ? g_lastAppliedMode : CP_LATCH_CLEAN,
          forceClean ? 1 : 0);
  LOG_DBG("GFX",
          "paintdec req=%d res=%d force=%d ratio=%d shadow=%d auto=%d promo=%d mode=%d valid=%d clean=%d prev=%d end=%d",
          static_cast<int>(requestedAsync), producerModeAsync, forceClean ? 1 : 0, g_paintDecisionRatio,
          shadowHadPrevAsync ? 1 : 0, autoArmedAsync ? 1 : 0, promoArmedAsync ? 1 : 0, promoModeAsync,
          g_lastAppliedValid ? 1 : 0, clean ? 1 : 0, g_lastAppliedValid ? g_lastAppliedMode : CP_LATCH_CLEAN,
          static_cast<int>(refreshMode));
#else
  refreshMode = applyPromotedRefresh(refreshMode);
  if (binaryDuForcedAsync) refreshMode = HalDisplay::FAST_REFRESH;
#endif
  // The async path has no turn-off-screen hook, which the sunlight fading fix
  // relies on; keep those users on the blocking path.
  if (fadingFix) {
    if (binaryDuForcedAsync) cpPanelArmBinaryDu();
    display.displayBuffer(refreshMode, fadingFix);
    return;
  }
  if (binaryDuForcedAsync) cpPanelArmBinaryDu();
  display.displayBufferAsync(refreshMode);
#if CP_RICKY_REFRESH_POLICY
  // B-P2-3: record this drive in the refresh latch. "Async" here is a CALLER-SIDE overlap, not a
  // deferred drive, on every panel this fork ships: HalDisplay::displayBufferAsync() runs the whole
  // waveform before returning wherever the controller has no displayStart, and says so by
  // supportsAsyncRefresh() == false (see the note at that call site). Writing no latch at all meant
  // the latch still described the last BLOCKING paint, so after an async page turn the next blocking
  // paint computed `clean` from a stale mode and could promote to HALF -- one slow, heavy repaint at
  // a random point.
  //
  // Row 7 still holds: the latch is written ONLY for a drive that reports success, and it is written
  // here only because this call already knows the result. Where the panel really defers the waveform
  // (supportsAsyncRefresh() true) there is no result yet; inventing one would be worse than the stale
  // latch, so that case is left exactly as it was and logged.
  if (!display.supportsAsyncRefresh()) {
    const uint32_t asyncResult = lastPanelDrawError();
    if (asyncResult == 0) {
      g_lastAppliedMode = (requestedAsync == HalDisplay::FULL_REFRESH) ? CP_LATCH_FULL : CP_LATCH_FAST;
      g_lastAppliedValid = true;
      // BATT5: "async" is a CALLER-SIDE overlap on every panel this fork ships -- the
      // whole waveform ran before this call returned (see the note in
      // HalDisplay::displayBufferAsync), so the driver accepted this frame and the
      // same evidence that writes the latch above re-bases the glass epoch. The reader
      // turns its pages through THIS entry, so without it the epochs would never meet
      // on a book page and the icon patch could not run there at all.
      glassEpochs_.noteDriveLanded();
#if CP_SKIP_REDRIVE
      g_lastDriveFromWhiteClean = forceClean && refreshMode == HalDisplay::FULL_REFRESH;
#endif
    } else {
      // Row 12, same as the blocking path: the glass did not take the frame, so the shadow describes
      // content the glass never got and the next paint must be a from-white FULL.
      g_lastAppliedMode = CP_LATCH_CLEAN;
      g_lastAppliedValid = false;
      // BATT5: fail closed, same as the blocking path.
      glassEpochs_.noteDriveFailed();
#if CP_SKIP_REDRIVE
      g_lastDriveFromWhiteClean = false;
#endif
      prevSampleValid_ = false;
      promotedRefresh_ = HalDisplay::FULL_REFRESH;
      promotedRefreshPending_ = true;
      autoRefreshPending_ = false;
      LOG_ERR("GFX", "paint async drive failed err=0x%x: shadow declared stale, next paint from-white",
              static_cast<unsigned>(asyncResult));
    }
    LOG_DBG("GFX", "paint async latch mode=%d valid=%d result=0x%x (drive completed inline)",
            g_lastAppliedMode, g_lastAppliedValid ? 1 : 0, static_cast<unsigned>(asyncResult));
  } else {
    LOG_DBG("GFX", "paint async latch unchanged: panel defers the drive, so there is no result to record");
  }
#endif  // CP_RICKY_REFRESH_POLICY
}

void GfxRenderer::waitRefreshComplete() const { display.waitRefreshComplete(); }

bool GfxRenderer::supportsAsyncRefresh() const { return !fadingFix && display.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities GfxRenderer::grayscaleCapabilities(HalDisplay::GrayscaleMode mode) const {
  auto caps = display.grayscaleCapabilities(mode);
  if (fadingFix) caps.asyncBase = false;
  return caps;
}

bool GfxRenderer::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  if (dst == nullptr || w <= 0 || h <= 0) return 0;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return 0;

  const size_t rowBytes = mem.w / pixfmt::kPxPerByte;  // exact: mem.w is a multiple of 8
  const size_t needed = rowBytes * mem.h;
  if (needed > dstCapacity) return 0;

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow =
        frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / pixfmt::kPxPerByte);
    uint8_t* dstRow = dst + (static_cast<size_t>(row) * rowBytes);
    memcpy(dstRow, srcRow, rowBytes);
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  if (src == nullptr || w <= 0 || h <= 0) return;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return;
  // BATT5: bulk canvas write.
  noteFramebufferWrite();

  const size_t rowBytes = mem.w / pixfmt::kPxPerByte;  // exact: mem.w is a multiple of 8

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = src + (static_cast<size_t>(row) * rowBytes);
    uint8_t* dstRow =
        frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / pixfmt::kPxPerByte);
    memcpy(dstRow, srcRow, rowBytes);
  }
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  size_t charCount = 0;
  for (const unsigned char c : item) {
    if ((c & 0xC0) != 0x80) ++charCount;
  }

  std::string candidate;
  candidate.reserve(item.size() + 3);
  const auto setCandidate = [&](const size_t characterCount) {
    size_t end = 0;
    for (size_t count = 0; end < item.size() && count < characterCount; ++count) {
      ++end;
      while (end < item.size() && (static_cast<unsigned char>(item[end]) & 0xC0) == 0x80) ++end;
    }
    candidate.assign(item.data(), end);
    candidate += ellipsis;
  };

  // Binary search avoids repeatedly measuring nearly the entire long title.
  size_t low = 0;
  size_t high = charCount;
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    setCandidate(mid);
    if (getTextWidth(fontId, candidate.c_str(), style) < maxWidth) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }

  setCandidate(low);
  return candidate;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else if (!currentLine.empty()) {
      lines.push_back(currentLine);
      currentLine.clear();
      // Requeue the word instead of deciding its fate here: the loop top then
      // applies the last-line ellipsis or the overlong split with the correct
      // remaining line budget.
      remaining = remaining.empty() ? word : word + " " + remaining;
    } else {
      // Single word wider than the line — a no-space filename or a CJK run,
      // which has no space boundaries at all. Hard-split at the widest UTF-8
      // prefix that fits (never below one codepoint, so the loop advances) and
      // requeue the rest to keep wrapping.
      size_t fit = 0;
      while (fit < word.size()) {
        size_t next = fit + 1;
        while (next < word.size() && (static_cast<unsigned char>(word[next]) & 0xC0) == 0x80) next++;
        if (fit > 0 && getTextWidth(fontId, word.substr(0, next).c_str(), style) > maxWidth) break;
        fit = next;
      }
      lines.push_back(word.substr(0, fit));
      const std::string rest = word.substr(fit);
      if (!rest.empty()) remaining = remaining.empty() ? rest : rest + " " + remaining;
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't. kPxPerByte is
  // 8 for the 1 bpp canvas and 2 for 4 bpp, so the same align-to-byte rule holds.
  const int byteX0 = x0 / pixfmt::kPxPerByte;
  const int byteX1 = x1 / pixfmt::kPxPerByte;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / pixfmt::kPxPerByte;
  const int byteX1 = x1 / pixfmt::kPxPerByte;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / pixfmt::kPxPerByte;
  const int byteX1 = x1 / pixfmt::kPxPerByte;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  // BATT5: bulk canvas write (copyRegionToBuffer, the reader above, only reads).
  noteFramebufferWrite();
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    return fp4::toPixel(getSdCardSpaceAdvance(*sdIt->second, style));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    return fp4::toPixel(getSdCardSpaceAdvance(*sdIt->second, style));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style, const int8_t tracking) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);      // 4.4 fixed-point
  return fp4::toPixel(kernFP) + trackingBetween(leftCp, rightCp, tracking);  // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style, const int8_t tracking,
                                 const BidiUtils::BidiBaseDir baseDir, const TextMeasureMode mode) const {
  // Match the font drawText would use for non-Latin strings (see resolveTextFontId).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  // Measure the exact codepoint stream drawText renders: bidi-reordered and
  // Arabic-shaped (contextual presentation forms, Lam-Alef collapse).
  // Measuring the raw logical text counts the Alef a ligature absorbs and
  // uses base-letter advances instead of presentation-form advances, so RTL
  // lines come out wider than they draw — uneven word gaps and a ragged
  // right margin.
  std::string visual;
  text = resolveVisualText(text, visual, baseDir);

  // Advance table fast-path for SD card fonts during layout.
  // Snap each advance like drawText. Kerning/ligature data is not resident
  // during layout, so kerned-pair steps can still differ.
  auto sdIt = sdCardFonts_.find(resolvedFontId);
  if (mode == TextMeasureMode::Layout && sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int widthPx = 0;
    int32_t prevAdvanceFP = 0;
    uint32_t prevCp = 0;
    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    const auto fontIt = fontMap.find(resolvedFontId);
    if (fontIt == fontMap.end()) {
      LOG_ERR("GFX", "Font %d not found", resolvedFontId);
      return 0;
    }
    const auto& font = fontIt->second;
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      // RTL vowel marks (niqqud/harakat) and combining marks are zero-advance overlays in drawText.
      if (BidiUtils::isTransparentMark(cp) || utf8IsCombiningMark(cp)) {
        continue;
      }
      const uint32_t smallCapCp = (style & EpdFontFamily::SMALL_CAPS) ? utf8SmallCapsUpper(cp) : cp;
      const bool isSmallCap = smallCapCp != cp;
      cp = smallCapCp;
      int32_t advFP = sdIt->second->getAdvance(cp, styleIdx);
      if (advFP == 0 || (syntheticGlyph::isSolid(cp) && !font.hasCodepoint(cp, style))) {
        EpdGlyph solidFallback;
        const EpdGlyph* glyph = font.getGlyphMetrics(cp, solidFallback, style);
        advFP = glyph ? glyph->advanceX : 0;
      }
      if (prevCp != 0) {
        widthPx += fp4::toPixel(prevAdvanceFP) + trackingBetween(prevCp, cp, tracking);
      }
      prevAdvanceFP = isSupSub ? (advFP + 1) / 2 : (isSmallCap ? scaleSmallCapAdvance(advFP) : advFP);
      prevCp = cp;
    }
    widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
    return widthPx;
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
    if (BidiUtils::isTransparentMark(cp)) {
      continue;
    }
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    const uint32_t smallCapCp = (style & EpdFontFamily::SMALL_CAPS) ? utf8SmallCapsUpper(cp) : cp;
    const bool isSmallCap = smallCapCp != cp;
    if (style & EpdFontFamily::SMALL_CAPS) {
      cp = smallCapCp;
    } else {
      cp = font.applyLigatures(cp, text, style);
    }

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP) + trackingBetween(prevCp, cp, tracking);
    }

    EpdGlyph solidFallback;
    const EpdGlyph* glyph = font.getGlyphMetrics(cp, solidFallback, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    } else if (isSmallCap) {
      prevAdvanceFP = scaleSmallCapAdvance(prevAdvanceFP);
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

bool GfxRenderer::getCodepointMetrics(const int fontId, const uint32_t cp, const EpdFontFamily::Style style,
                                      int32_t& advanceFP, int& top) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return false;
  const EpdGlyph* glyph = fontIt->second.getGlyph(cp, style);
  if (!glyph) return false;
  advanceFP = glyph->advanceX;
  top = glyph->top;
  return true;
}

int GfxRenderer::drawScaledCodepoint(const int fontId, const uint32_t cp, const EpdFontFamily::Style style, const int x,
                                     const int baselineY, const int scale256) const {
  if (scale256 <= 0) return 0;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    char utf8[5] = {};
    utf8AppendCodepoint(utf8, cp);
    fontCacheManager_->recordText(utf8, fontId, style);
    return 0;
  }
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const EpdFontFamily& font = fontIt->second;
  const EpdGlyph* glyph = font.getGlyph(cp, style);
  if (!glyph) return 0;
  const int advance = fp4::toPixel(static_cast<int32_t>(glyph->advanceX) * scale256 / 256);
  const EpdFontData* fontData = font.getData(style);
  const uint8_t* bitmap = getGlyphBitmap(fontData, glyph);
  if (!bitmap) return advance;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const bool is2Bit = fontData->is2Bit;
  // Raw coverage: 1-bit fonts read 0 or 3, 2-bit fonts 0 (white) .. 3 (black); 0 outside the bitmap.
  const auto sample = [&](const int sx, const int sy) -> int {
    if (sx < 0 || sy < 0 || sx >= srcW || sy >= srcH) return 0;
    const int pos = sy * srcW + sx;
    if (is2Bit) return (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
    return ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

  const int dstW = (srcW * scale256 + 255) / 256;
  const int dstH = (srcH * scale256 + 255) / 256;
  const int left = x + glyph->left * scale256 / 256;
  const int top = baselineY - glyph->top * scale256 / 256;
  // Source position of each destination pixel centre, 16.16 fixed point.
  const int32_t step = (256 << 16) / scale256;
  const int32_t origin = step / 2 - (1 << 15);
  for (int dy = 0; dy < dstH; ++dy) {
    const int32_t sy = origin + dy * step;
    const int y0 = sy >> 16;
    const int fy = (sy >> 8) & 0xFF;
    for (int dx = 0; dx < dstW; ++dx) {
      const int32_t sx = origin + dx * step;
      const int x0 = sx >> 16;
      const int fx = (sx >> 8) & 0xFF;
      const int upper = sample(x0, y0) * (256 - fx) + sample(x0 + 1, y0) * fx;
      const int lower = sample(x0, y0 + 1) * (256 - fx) + sample(x0 + 1, y0 + 1) * fx;
      const int cov = upper * (256 - fy) + lower * fy;  // 0 .. 3*65536
      if constexpr (pixfmt::kFourBit) {
        // Drop caps are bilinearly resampled, so their interpolated coverage is
        // already fractional. Quantise it to the same 4 levels as every other
        // glyph instead of thresholding at half black — a decapitated cap gets
        // the same anti-aliased edge as body text.
        int level = cov * 4 / (3 * 65536 + 1);
        if (level > 3) level = 3;
        if (level == 0) continue;
        drawPixelLevel(left + dx, top + dy,
                       greyscaleAdmitted() ? pixfmt::glyphLevelToNibble(static_cast<uint8_t>(level))
                                           : pixfmt::kInk);
      } else if (cov * 2 >= 3 * 65536) {
        // Ink where interpolated coverage reaches half of full black (3).
        drawPixel(left + dx, top + dy, true);
      }
    }
  }
  return advance;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(const int fontId, const float compression) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return static_cast<int>(fontIt->second.getSizeInPixels() * compression + 0.5f);
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  // Route non-Latin strings to the fallback font (see resolveTextFontId).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  // Redirected to the SD fallback: batch-load the string's glyphs so the draw
  // loop below doesn't fault them in one SD read at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, text, style, false);
  }
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before.
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::anchorOverRotated90CW(anchor, lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    EpdGlyph solidFallback;
    const EpdGlyph* glyph = font.getGlyphMetrics(cp, solidFallback, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback) const {
  absoluteGrayPlanes = false;
  // BATT5: deliberately NOT an epoch re-base. This entry drives the panel below the
  // paint machinery, with no error code and no rail verdict, so there is no evidence
  // that the frame landed -- and inventing one is exactly what the glass epoch exists
  // to prevent. The epoch stays behind, CP_ICON_SAFE_PATCH refuses, and the glyph
  // waits for the next paint that does carry a verdict. Fail closed, not open.
  display.displayGrayscaleBase(fallback, fadingFix);
}

bool GfxRenderer::displayGrayscaleBase(HalDisplay::GrayscaleMode mode, HalDisplay::RefreshMode fallback) const {
  absoluteGrayPlanes = false;
  if (!display.displayGrayscaleBase(mode, fallback, fadingFix)) return false;
  absoluteGrayPlanes = mode != HalDisplay::GrayscaleMode::Overlay;
  return true;
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const { display.copyGrayscaleLsbBuffers(frameBuffer); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { display.copyGrayscaleMsbBuffers(frameBuffer); }

void GfxRenderer::displayGrayBuffer() const {
  display.displayGrayBuffer(fadingFix);
  absoluteGrayPlanes = false;
}

void GfxRenderer::setRenderMode(RenderMode mode) {
  if (mode == BW && absoluteGrayPlanes) {
    display.cleanupGrayscaleBuffers(nullptr);  // cancel an unfinished absolute pass
    absoluteGrayPlanes = false;
  }
  renderMode = mode;
}

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool GfxRenderer::combinesGrayscaleBase() const {
  return grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined;
}

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer(const bool resyncPanelBaseline) {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  // BATT5: this restores the whole canvas from the stored BW copy.
  noteFramebufferWrite();
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  if (resyncPanelBaseline) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    display.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  // Board truth: the bezel insets live in the active profile (panel-native
  // portrait frame); this only rotates them into the current orientation.
  const BoardConfig::ViewableInsets& vi = BoardConfig::ACTIVE.viewableInsets;
  switch (orientation) {
    case Portrait:
      *outTop = vi.top;
      *outRight = vi.right;
      *outBottom = vi.bottom;
      *outLeft = vi.left;
      break;
    case LandscapeClockwise:
      *outTop = vi.left;
      *outRight = vi.top;
      *outBottom = vi.right;
      *outLeft = vi.bottom;
      break;
    case PortraitInverted:
      *outTop = vi.bottom;
      *outRight = vi.left;
      *outBottom = vi.top;
      *outLeft = vi.right;
      break;
    case LandscapeCounterClockwise:
      *outTop = vi.right;
      *outRight = vi.bottom;
      *outBottom = vi.left;
      *outLeft = vi.top;
      break;
  }
}
