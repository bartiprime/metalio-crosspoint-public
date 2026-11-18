#pragma once

#include "ContentRole.h"
#include "GlassEpoch.h"

#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;
class TtfEpdFont;

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "FontVariantProvider.h"
#include "PixFormat.h"

// ---------------------------------------------------------------------------
// r101.9 WAKE3 — WHO PAINTED THIS DRIVE. Diagnostics only: these four symbols
// write/read two integers and a 48-byte string, and nothing in the drive or
// policy path branches on them, so adding them cannot add, drop, re-mode or
// re-time a drive.
//
// The `[WAKE] drive N` line is emitted from the panel journal hook (main.cpp),
// i.e. BELOW the renderer, so it can never say which paint issued the waveform.
// The stamp travels the other way:
//   * ActivityManager's render task calls cp_paint_owner_note(<activity>) right
//     before Activity::render() and cp_paint_owner_clear() right after it, so a
//     drive that happens inside a committed paint knows its screen;
//   * GfxRenderer counts displayBuffer()/displayBufferAsync() ENTRIES;
//   * the journal hook prints `p<N>/R:<activity>` for a render-task paint and
//     `p<N>/M` for any other task (an Activity::onEnter()/first-render paint, a
//     follow-up, the shell, the power-off path).
// Two drive lines carrying the same N came out of ONE paint entry (= one
// displayBuffer call that drove twice); two different N are two paints, which
// is what "who issued the second drive" is asking.
extern "C" void cp_paint_owner_note(const char* activity);
extern "C" void cp_paint_owner_clear(void);
extern "C" const char* cp_paint_origin_text(void);
extern "C" uint32_t cp_paint_seq(void);

namespace glyphBitmap {
struct Frame;
}

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  // BATT5: the battery glyph's rectangle, recorded by the paint that draws it.
  //
  // PUBLIC because the rect and the epochs that license using it are one fact handed
  // to src/display/BatteryIconPatch.cpp; the MEMBER stays private below.
  //
  // `epoch` is 0 until the paint that recorded the rect is CONFIRMED to have driven
  // (ActivityManager calls commitBatteryIconRect(drove) at the end of the paint), and
  // is then set to the compose epoch of that frame -- i.e. to the value the glass
  // epoch takes when the drive lands. So `epoch == glassEpoch` says the rect belongs
  // to the frame the panel is holding, and a rect drawn by a paint that was held,
  // declined or failed can never satisfy it.
  struct BatteryIconRect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    bool valid = false;   // a paint has recorded a rect since the last reset
    uint32_t epoch = 0;   // compose epoch of the DROVE paint that recorded it (0 = uncommitted)
  };
  // BATT6: the percentage NUMBER's rectangle, recorded by the paint that draws it.
  //
  // Same contract as BatteryIconRect above and for the same reason: the box and
  // the epochs that license using it are one fact, so they live on the object that
  // owns the epochs. `value` is the percentage the DROVE paint drew into that box,
  // which is what makes "the glass already reads N %" a claim about the panel
  // rather than about our intentions.
  //
  // `align` says where anchorX sits: Left = the pen origin (text grows right),
  // Right = the text's right edge (text grows left, as every status cluster that
  // right-aligns the number does).
  struct BatteryPercentRect {
    enum Align : uint8_t { Left = 0, Right = 1 };
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;         // the drawn text's width (the laid-out line box's height is h)
    int16_t h = 0;
    int16_t anchorX = 0;
    int16_t fontId = -1;
    int16_t value = -1;    // the percentage this box was painted with
    uint8_t align = Left;
    bool valid = false;    // a paint has recorded a box since the last reset
    uint32_t epoch = 0;    // compose epoch of the DROVE paint that recorded it (0 = uncommitted)
  };
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // r163 D3 -- WHAT A PIXEL IS PART OF. The table lives in the sibling ContentRole.h
  // (host-tested by test/grey_role); this is the alias the renderer's API speaks.
  //   Untagged        the legacy default: the paint's own intent (textBinary_) decides.
  //   Monochrome      a page of text/line art: may carry intermediate levels.
  //   FastMonochrome  the same content on the fast path (a page turn).
  //   Shelf           a library/shelf grid and its slots.
  //   Cover           a book cover image.
  //   Illustration    any other visible artwork (a logo, a plate).
  //   Text            a text blit outside the reader's own path.
  //   Icon            a status/UI glyph: NEVER grey, whatever drew it.
  using ContentRole = gfxcontent::Role;

  // The admission table, in one place: only `Icon` denies. A role can never GRANT grey
  // (see greyscaleAdmitted()), so no tag can make an element newly ghost.
  static constexpr bool roleAdmitsGrey(const ContentRole role) { return gfxcontent::admitsGreyscale(role); }

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode = BW;                       // uninitialised -> UB (first enumerator, matches observed behaviour)
  mutable bool absoluteGrayPlanes = false;
  // GRAY16 opt-in for drawBitmap(): the sleep screen sets it around its own paint
  // so a BMP's 16 levels reach the canvas, and every other consumer of drawBitmap
  // (library thumbnails, menus, the BMP viewer) keeps the 2-bit halftone path --
  // a thumbnail must not drag a menu paint onto the grey waveform.
  mutable bool imageLevels16 = false;
  // NATIVE panel orientation by default: tapToLogical() treats this case as the
  // identity, and this board's display path is the identity too. The member had no
  // initialiser at all, so every tap before the first setOrientation() (sleep/reader)
  // mapped through an INDETERMINATE enum value -- a coin flip among four rotations.
  Orientation orientation = Orientation::LandscapeCounterClockwise;
  bool fadingFix = false;                          // uninitialised -> UB
  uint8_t* frameBuffer = nullptr;
  uint32_t frameBufferLoans = 0;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;
  mutable std::map<int, uint16_t> sdCardFontScales_;  // fontId -> 8.8 fixed point scale (256=1.0x)
  // TTF (vector) fonts: rebuilt per page by ensureSdCardFontReady(). Mutable for
  // the same reason as sdCardFonts_ (const layout path triggers a rebuild).
  mutable std::map<int, TtfEpdFont*> ttfFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // One-shot refresh promotion (see promoteNextRefresh). Mutable because
  // displayBuffer() is const but must consume the flag.
  mutable bool promotedRefreshPending_ = false;
  mutable HalDisplay::RefreshMode promotedRefresh_ = HalDisplay::FAST_REFRESH;
  // FLP1 one-shot: suppress the row-9 completion drive for exactly the next
  // paint. Mutable because displayBuffer() is const but must consume the flag.
  mutable bool suppressCompletionOnce_ = false;
  // FLP2 one-shot: the next paint reaches the glass as a diff-masked DU drive
  // AND is exempt from the shim's grey lane / clean cadence (see
  // forceBinaryDuOnce below). Mutable for the same reason as the two above.
  mutable bool binaryDuOnce_ = false;
  // BATT5: the "this canvas IS the frame on the glass" bookkeeping (GlassEpoch.h).
  //
  // `composed` is bumped by noteFramebufferWrite() from EVERY sink that mutates the
  // canvas; `glass` is assigned from it at the single point below where a drive is
  // ACCEPTED (displayBuffer()/displayBufferAsync() success). Equality is the licence
  // that lets CP_ICON_SAFE_PATCH touch a region in place: it means no composition has
  // happened since the frame the panel is holding. Mutable because the const drawing
  // API is what mutates the canvas.
  mutable cpglass::Epochs glassEpochs_;
  mutable BatteryIconRect batteryIconRect_;
  mutable BatteryPercentRect batteryPercentRect_;
  // r161 MENU AA: when set, every glyph blit is a BINARY mask (covered pixel ->
  // ink, nothing in between), so the composed frame carries no intermediate
  // optical level and the panel takes its binary DU lane. Mutable because
  // drawText() is const. Read by drawGlyphBitmap, renderCharScaled and
  // drawScaledCodepoint; false (the default) is the AA blit every earlier build
  // shipped.
  mutable bool textBinary_ = false;
  // r163 D3: CONTENT ROLE, the admission tag for the grey lane.
  //
  // `textBinary_` is CALLER INTENT ("this paint wants no intermediate levels").
  // This is CONTENT ("what kind of thing is this pixel part of"). The two are
  // independent: a role can only ever DENY grey, never grant it, so nothing that is
  // binary today becomes grey because of a tag. Untagged (the default, and every
  // call site not listed below) behaves exactly as before -- the paint's own
  // intent decides.
  //
  // Icons are the reason this exists: a status glyph is binary by ROLE regardless
  // of what source drew it (a 4 bpp blit, a scaled codepoint, an AA primitive), so
  // an icon can never ghost on the grey lane because someone changed how it is
  // sourced.
  mutable ContentRole contentRole_ = ContentRole::Untagged;
  // Swap in (and clear) the promoted mode, if one is pending.
    HalDisplay::RefreshMode applyPromotedRefresh(HalDisplay::RefreshMode refreshMode) const;
    // The same producer as applyPromotedRefresh but without the drive: returns the mode
    // the request resolves to (measured AUTO ratio first, then the armed promotion).
    // Only defined when CP_RICKY_REFRESH_POLICY is 1; see src/display/RefreshPolicy.h.
    HalDisplay::RefreshMode resolveRefreshRequest(HalDisplay::RefreshMode refreshMode) const;

  // "Auto": decide at display time from how much of the frame changed. Small edits stay on DU;
  // anything larger is cleaned. r101.5: the shadow is FULL SIZE (one byte per framebuffer byte,
  // ~416 KB at this geometry) so the comparison is exact; the 1-in-8 sampled shadow (13 KB for a
  // 104 KB frame) is kept only as the fallback when the full allocation fails. Either way it is one
  // buffer allocated once, in ensureDiffShadow().
  mutable bool autoRefreshPending_ = false;
  // owned, PSRAM. bufferSize when the exact shadow is active, bufferSize/8 in the sampled fallback.
  mutable uint8_t* prevSampleShadow_ = nullptr;
  // Allocates the shadow on first use (exact, else sampled) and logs which mode is active once.
  bool ensureDiffShadow() const;
  mutable bool prevSampleValid_ = false;
  // r96.2 (D1/D2): set by displayBuffer() to whether THAT paint consumed the forced
  // clean-on-arrival one-shot. Read after the call (reader arm verification, Home's
  // wake one-drive suppression).
  mutable bool lastPaintForcedClean_ = false;
  // r97.2 CP_ENTRY_SINGLE_DRIVE: the entry gate. Armed by ActivityManager around onEnter(),
  // released when the screen's content is built. displayBuffer() returns without driving (and
  // without consuming any one-shot) while it is armed and inside its budget.
  mutable bool entryGateArmed_ = false;
  mutable uint32_t entryGateDeadlineMs_ = 0;
  mutable bool entryGateHeldPaint_ = false;  // a paint was suppressed; it must be repaid
  mutable bool entryGateHeldLast_ = false;   // the LAST displayBuffer() call was held
  // r97.2 CP_SKIP_REDRIVE: read-only form of the existing sampled diff. 1 = the framebuffer is
  // byte-identical to the frame the shadow last committed (i.e. to what the glass holds, when the
  // last drive was a from-white clean of that frame), 0 = it changed, -1 = no baseline yet.
  int framebufferSameAsShadow() const;
  // Share of sampled bytes that differ from the last displayed frame, 0-100 (-1 = unknown).
  int changedRatioPercent() const;
  // Below this percentage Auto stays on DU. Set from SETTINGS by Activity::requestUpdate.
  mutable int autoRefreshThresholdPercent_ = 5;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;
  mutable int clipLeft_ = 0;
  mutable int clipTop_ = 0;
  mutable int clipRight_ = 32767;
  mutable int clipBottom_ = 32767;

  // CJK UI font fallback map: primary (built-in, Latin-only) UI font id -> a
  // size-matched SD-card font id that carries CJK glyphs. When a string drawn
  // or measured with a mapped primary font contains a CJK codepoint the primary
  // cannot render, the whole string is routed to the mapped fallback so it
  // appears at the same point size as the surrounding UI text. Populated by the
  // app-level SD font setup when an SD family is loaded. See resolveTextFontId().
  std::map<int, int> fallbackFontMap_;
  FontVariantProvider fontVariantProvider_;

  // If `text` contains a CJK codepoint that `fontId` cannot render and `fontId`
  // has a registered fallback, returns the fallback id; otherwise returns
  // fontId unchanged. The whole string is routed as a unit so each draw/measure
  // call stays single-font (consistent bit depth, metrics, wrapping).
  int resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const;

  // Batch-load `text`'s glyphs into an SD-card font's resident mini tables
  // before a per-glyph measure/draw loop runs. Called when resolveTextFontId
  // redirected a string to the SD fallback: UI screens (file browser, home)
  // draw those strings without the reader's PrewarmScope, and every glyph
  // would otherwise fault through SdCardFont::onGlyphMiss — one .cpfont file
  // open + seek + read per glyph, per redraw, through an 8-slot overflow ring
  // (#2725). One prewarm per string costs a single file open; re-measuring or
  // re-drawing resident glyphs is a RAM-only subset check. No-op for built-in
  // fonts.
  void ensureSdGlyphsResident(int fontId, const char* text, EpdFontFamily::Style style, bool metadataOnly) const;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
    sdCardFontScales_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  // Batch-prewarm CJK fallback glyphs for a screenful of static strings in ONE
  // SD pass. List screens redraw every visible row on each repaint; without an
  // up-front batch each row's draw prewarms per-string, and under heap
  // pressure (union merge disabled) each string evicts the previous one — SD
  // reads on every repaint forever. Call once when the screen's strings are
  // known (data load); later measures/draws become RAM-only subset hits.
  // No-op when nothing routes to an SD fallback.
  // The getter form fetches strings one at a time (allocation-free — callers
  // must NOT build a concatenated std::string: its bare-new growth aborts on
  // the heap-tight screens this exists for). A null getter result skips that
  // index.
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);
  void prewarmFallbackText(int fontId, TextGetter getter, const void* ctx, uint32_t textCount,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void prewarmFallbackText(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    sdCardFontScales_.clear();
  }
  void registerSdCardFontScale(int fontId, uint16_t scale) { sdCardFontScales_[fontId] = scale; }
  void clearSdCardFontScales() { sdCardFontScales_.clear(); }
  uint16_t getSdCardFontScale(int fontId) const {
    auto it = sdCardFontScales_.find(fontId);
    return (it != sdCardFontScales_.end()) ? it->second : 256;
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // TTF (vector) fonts rendered via TtfEpdFont/FreeInkFont. Registered like an
  // ordinary EpdFontFamily (insertFont), plus tracked here so ensureSdCardFontReady()
  // rebuilds their per-page glyph set on demand — the eager analogue of the SD
  // font prewarm. The TtfEpdFont is owned by the caller (SdCardFontSystem).
  void registerTtfFont(int fontId, TtfEpdFont* font) { ttfFonts_[fontId] = font; }
  void unregisterTtfFont(int fontId) { ttfFonts_.erase(fontId); }
  const std::map<int, TtfEpdFont*>& getTtfFonts() const { return ttfFonts_; }
  // Register/clear size-matched CJK UI fallbacks (see fallbackFontMap_).
  // setFallbackFont maps a primary UI font id to an SD font id of the same size.
  void setFallbackFont(int primaryFontId, int fallbackFontId) { fallbackFontMap_[primaryFontId] = fallbackFontId; }
  void clearFallbackFonts() { fallbackFontMap_.clear(); }

  void setFontVariantProvider(const FontVariantProvider& provider) { fontVariantProvider_ = provider; }
  int resolveFontVariant(const int fontId, const float scale) const {
    return fontVariantProvider_.resolveFont(fontId, scale);
  }
  // True when fontId is registered, loading it through the variant provider if needed.
  bool ensureFontLoaded(const int fontId) const {
    if (fontMap.count(fontId) != 0) return true;
    return fontVariantProvider_.load && fontVariantProvider_.load(fontVariantProvider_.ctx, fontId) &&
           fontMap.count(fontId) != 0;
  }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  // Packed variant for the paragraph layout path: each segment holds
  // consecutive NUL-terminated words (WordStore chunks), so a whole paragraph
  // is scanned without materializing per-word strings.
  void ensureSdCardFontReady(int fontId, const char* const* segments, const size_t* segmentLens, size_t segmentCount,
                             bool includeSpace, bool includeHyphen, uint8_t styleMask = 0x0F,
                             bool includeSmallCaps = false) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }
  // Logical (orientation-aware) -> physical panel coordinates: the transform every
  // framebuffer write goes through. Public so an exporter (ScreenshotUtil) can
  // invert it instead of restating the table and drifting from it.
  static void logicalToPhysical(Orientation o, int x, int y, uint16_t panelWidth, uint16_t panelHeight, int* phyX,
                                int* phyY);

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // One-shot: the next displayBuffer()/displayBufferAsync() call uses `mode`
  // instead of what its caller asked for, then the override clears itself.
  // Lets a closing overlay (the control center's refresh tile) hand a
  // ghost-cleanup waveform to the repaint of whatever screen is underneath,
  // which it cannot reach directly.
  void promoteNextRefresh(const HalDisplay::RefreshMode mode) const {
    promotedRefreshPending_ = true;
    autoRefreshPending_ = false;
    promotedRefresh_ = mode;
  }
  // One-shot: the next paint is driven as a clean from-white FULL (CP_RM_FULL) no matter what its
  // call site or the armed promotion asked for. NOT a second decision path: it is a term the
  // existing policy in displayBuffer() reads, and it is consumed exactly once, before the drive.
  // r93 arms it from ActivityManager::goHome() when Home is reached from the reader -- a book's
  // anti-aliased greys and the GL16 page turns before them leave traces that Home would otherwise
  // inherit. Cost: one whole-panel from-white GC16 (~815 ms) on that transition, once.
  void forceCleanNextRefresh() const;
  // r96.2 (D1): did the paint that just returned take the forced clean? Read by the
  // reader's image-page arm so a clean that was armed but never consumed is caught at
  // the call site instead of vanishing (r96.1: the operator saw it fire on neither
  // edge precisely because nothing checked).
  bool lastPaintForcedClean() const { return lastPaintForcedClean_; }
  // r96.2 (D1): how many paints have consumed a forced clean since boot. A call site
  // that arms one-shot cleans samples this before and after its drive: if it did not
  // move, the arm was not spent and must not be recorded as done.
  uint32_t forcedCleanPaintsConsumed() const;
  // r97 MAP1: is a forced clean ARMED right now (and not yet consumed)? Read-only view of
  // the same one-shot forceCleanNextRefresh() sets. ActivityManager's `[MAP]` transition
  // line uses it so the "clean issued?" column of the non-list transition map can be read
  // off the device log instead of inferred from which screen came next.
  bool forcedCleanArmed() const;
  // r97.2 CP_ENTRY_SINGLE_DRIVE: the entry gate. ActivityManager arms it around the incoming
  // activity's onEnter() and releases it once the screen's content exists; while armed,
  // displayBuffer() neither drives nor consumes any one-shot (promotion, forced clean, AUTO,
  // diff shadow) -- the drive it held happens after release, over the complete frame.
  // Bounded by CP_ENTRY_GATE_TIMEOUT_MS: past the budget the gate opens and the frame is driven
  // incomplete, so a slow or missing content source can never strand the screen.
  void holdEntryDrive(uint32_t timeoutMs) const;
  // Returns true when a paint was HELD: the caller must request one repaint to pay it back.
  bool releaseEntryDrive() const;
  bool entryGateArmed() const;
  // r97.2: did the LAST displayBuffer() call return without driving because the entry gate was
  // holding it? A screen that keys its "first paint" special-case off its own state (a list's HALF
  // entry drive) must not let a held paint consume it: the frame never reached the glass.
  bool lastPaintAttemptHeld() const;
  // FLP1: one-shot, consumed by the next paint that reaches the drive decision.
  // The row-9 completion is a SECOND whole-panel GL16 drive, which is what reads
  // as a flash on a panel that is being dragged: a call site that has already
  // committed to one FAST/DU drive arms this so its paint stays a single drive.
  // It is a term ANDed onto the existing #if CP_RICKY_COMPLETION_DRIVE test, not
  // a second decision path, and it never suppresses anything but that one paint.
  void suppressCompletionDriveOnce() const { suppressCompletionOnce_ = true; }
  // FLP2: the next paint that reaches the drive decision is forced to FAST (DU)
  // after the policy stage, and the vendor shim is told that paint is binary so
  // it cannot pick the 38-phase grey-wipe lane (metalio_vendor_panel.cpp:644)
  // or spend the 24-partial clean cadence (:719-726) on it.
  //
  // WHY A PIN AND NOT A PROMOTION: a promotion is a producer-stage input, and
  // cp_refresh_resolve_request() lets an armed AUTO outrank it (:385) and
  // cp_refresh_resolve_mode()'s rule 3 can still turn FAST into HALF after a
  // reader that left the latch at FULL (:415-417). The panel's adjustment is a
  // 1-bit chrome diff, so this one-shot is a term read AFTER the policy, and it
  // belongs to exactly one paint -- the same contract as the FLP1 one-shots.
  void forceBinaryDuOnce() const { binaryDuOnce_ = true; }
  // Are either of the two one-shots above still set? The arming side asks this when
  // its compose-epoch witness says the intent is over (a compose began since the arm,
  // or the deadline passed), to log and clear a pair that compose refused -- see
  // src/display/OneShotPaintPins.h. A pair a paint consumed answers false.
  bool oneShotPinsPending() const { return binaryDuOnce_ || suppressCompletionOnce_; }
  // Drop both, unconditionally. Only for a pin pair that was armed for a paint
  // which never reached the drive decision: clearing a pair the paint already
  // consumed is a no-op.
  void clearOneShotPins() const {
    binaryDuOnce_ = false;
    suppressCompletionOnce_ = false;
  }
  // -------------------------------------------------------------------------
  // BATT5 — IS THE CANVAS THE FRAME ON THE GLASS? (lib/GfxRenderer/GlassEpoch.h)
  //
  // The ONLY legitimate reason to touch a region of the canvas and drive it is
  // knowing that every OTHER pixel of the canvas already equals the glass: then
  // the panel's own framebuffer-vs-glass diff is that region and nothing else,
  // and no framebuffer can disagree with the panel about what is on it.
  //
  // `framebufferIsGlassFrame()` is that fact, and it is a POSITIVE one:
  //   noteFramebufferWrite()  called by every sink that mutates the canvas
  //   (drive accepted)        glassEpochs_.noteDriveLanded(), inside the
  //                           `result == 0` branch of displayBuffer() -- the same
  //                           branch that writes the refresh latch, i.e. the same
  //                           evidence the rest of the renderer already trusts
  //   (drive rejected/unknown) noteDriveFailed(), which ADVANCES `composed` so a
  //                           failed or deferred drive can never leave the two equal
  // A false EQUAL is impossible by construction: `glass` is only ever assigned
  // from `composed`, never synthesised. A false UNEQUAL is free -- the caller
  // falls back to the whole-activity repaint.
  //
  // External writers (main.cpp restores a saved frame through getFrameBuffer())
  // must call noteFramebufferWrite() themselves; that is the only way to break
  // the equality, and it breaks it in the safe direction if forgotten only when
  // the write is part of a paint that drives anyway.
  // -------------------------------------------------------------------------
  void noteFramebufferWrite() const { glassEpochs_.noteCanvasWrite(); }
  uint32_t framebufferComposeEpoch() const { return glassEpochs_.composed; }
  uint32_t framebufferGlassEpoch() const { return glassEpochs_.glass; }
  bool framebufferIsGlassFrame() const { return glassEpochs_.canvasIsGlass(); }
  // BATT5: the battery glyph's rectangle, for CP_ICON_SAFE_PATCH.
  //
  //   resetBatteryIconRect()   ActivityManager, at the TOP of every activity paint.
  //                            A screen that never reaches drawBatteryOutline()
  //                            therefore has no rect, which is the whole "is the
  //                            icon on this screen" test -- one store per paint.
  //   noteBatteryIconRect()    BaseTheme::drawBatteryOutline(), while composing.
  //   commitBatteryIconRect()  ActivityManager, at the END of the paint, with its
  //                            own `drove` verdict. Only a paint that reached the
  //                            glass may stamp the epoch the gate compares against.
  void resetBatteryIconRect() const {
    batteryIconRect_.valid = false;
    batteryIconRect_.epoch = 0;
  }
  void noteBatteryIconRect(const int x, const int y, const int w, const int h) const {
    batteryIconRect_.x = static_cast<int16_t>(x);
    batteryIconRect_.y = static_cast<int16_t>(y);
    batteryIconRect_.w = static_cast<int16_t>(w);
    batteryIconRect_.h = static_cast<int16_t>(h);
    batteryIconRect_.valid = w > 0 && h > 0;
    batteryIconRect_.epoch = 0;  // uncommitted until the paint proves it drove
  }
  void commitBatteryIconRect(const bool drove) const {
    if (!batteryIconRect_.valid) return;
    // A held / declined paint leaves the epoch at 0: the rect belongs to a frame the
    // glass never took, and the gate refuses it. A paint that drove but FAILED is
    // caught one step later by glassEpoch not advancing to this value.
    batteryIconRect_.epoch = drove ? glassEpochs_.composed : 0;
  }
  const BatteryIconRect& batteryIconRect() const { return batteryIconRect_; }
  // BATT6: the percentage NUMBER's box, for the change producer
  // (src/display/BatteryPercentPatch.cpp). Same three-step lifecycle as the icon's:
  //
  //   resetBatteryPercentRect()   ActivityManager, at the TOP of every activity paint.
  //                               A screen that draws no percentage therefore has no
  //                               box, which is the whole "is the number on this
  //                               screen" test -- one store per paint.
  //   noteBatteryPercentRect()    every site that draws the number, while composing.
  //   commitBatteryPercentRect()  ActivityManager, at the END of the paint, with its
  //                               own `drove` verdict. Only a paint that reached the
  //                               glass may stamp the epoch and the value.
  void resetBatteryPercentRect() const {
    batteryPercentRect_.valid = false;
    batteryPercentRect_.epoch = 0;
  }
  void noteBatteryPercentRect(const BatteryPercentRect& r) const { batteryPercentRect_ = r; }
  // BATT6: "this paint drew the percentage somewhere this path cannot address" --
  // the FreeInkUI header band (components/TitledHeaderBand.h), where the number is
  // drawn inside the SDK component and the app has no box of its own to repaint.
  //
  // The VALUE is recorded because it is the only claim that matters afterwards: the
  // change producer compares the gauge against it and, on a real change, asks the
  // activity for one whole-activity repaint (pinned to DU, so the panel still only
  // drives the pixels that differ). An unaddressable box is never patched in place.
  void noteBatteryPercentUnaddressable(const int value) const {
    batteryPercentRect_ = BatteryPercentRect{};
    batteryPercentRect_.valid = true;
    batteryPercentRect_.value = static_cast<int16_t>(value);
    // rectPlausible is derived from the geometry, so a record with no geometry is
    // exactly the unaddressable case: boxPlausible() sees w == 0 / fontId < 0.
    batteryPercentRect_.fontId = -1;
  }
  void commitBatteryPercentRect(const bool drove) const {
    if (!batteryPercentRect_.valid) return;
    // Same rule as the icon's: a held / declined paint leaves epoch 0, which the
    // producer's gate refuses. `value` is kept -- it describes the box, and the
    // epoch is what says whether the box is on the glass.
    batteryPercentRect_.epoch = drove ? glassEpochs_.composed : 0;
  }
  const BatteryPercentRect& batteryPercentRect() const { return batteryPercentRect_; }
  // Drop any armed one-shot promotion. A call site that must commit a specific waveform -- a forced
  // full scrub, a screen painted as the device powers off -- calls this immediately before
  // displayBuffer(mode), because applyPromotedRefresh() otherwise overrides the caller's mode with the
  // armed one and a stale AUTO/HALF flag silently downgrades the drive to a diff-area one.
  void cancelPromotedRefresh() const {
    promotedRefreshPending_ = false;
    autoRefreshPending_ = false;
  }
  void setAutoRefreshThreshold(const int percent) const {
    if (percent > 0 && percent <= 100) autoRefreshThresholdPercent_ = percent;
  }
  // "Auto": let the next displayBuffer() pick Fast or Half from the changed fraction.
  void promoteAutoRefresh() const {
    autoRefreshPending_ = true;
    promotedRefreshPending_ = false;
  }
  // True when a call site has already chosen a mode for the next paint, so a policy default must not
  // overwrite it.
  bool hasPromotedRefresh() const { return promotedRefreshPending_ || autoRefreshPending_; }
  // Peek/re-arm the armed one-shot promotion. A caller that must commit its own waveform -- a popup
  // drawn over a frame that still needs its FULL -- parks this value, commits, then re-arms, instead
  // of silently consuming the promotion on its own paint.
  struct ArmedPromotion {
    bool armed = false;
    bool autoMode = false;
    HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  };
  ArmedPromotion peekPromotedRefresh() const {
    return ArmedPromotion{hasPromotedRefresh(), autoRefreshPending_, promotedRefresh_};
  }
  void rearmPromotedRefresh(const ArmedPromotion& parked) const {
    if (!parked.armed) return;
    if (parked.autoMode) {
      autoRefreshPending_ = true;
      promotedRefreshPending_ = false;
      return;
    }
    promotedRefreshPending_ = true;
    autoRefreshPending_ = false;
    promotedRefresh_ = parked.mode;
  }
  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  // True when the display can overlap an ordinary B/W refresh with grayscale
  // composition without bypassing a required grayscale base waveform.
  HalDisplay::GrayscaleCapabilities grayscaleCapabilities(
      HalDisplay::GrayscaleMode mode = HalDisplay::GrayscaleMode::Overlay) const;
  // Compatibility queries for Overlay mode.
  bool supportsAsyncGrayscaleBase() const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  // UI drawing clip in logical coordinates; independent of panel orientation.
  std::array<int, 4> getClipRect() const {
    return {clipLeft_, clipTop_, clipRight_ - clipLeft_, clipBottom_ - clipTop_};
  }
  void setClipRect(int x, int y, int width, int height) const {
    clipLeft_ = x;
    clipTop_ = y;
    clipRight_ = x + width;
    clipBottom_ = y + height;
  }
  void drawPixel(int x, int y, bool state = true) const;
  // Write an explicit 4 bpp level (0x0 ink .. 0xF paper) at one pixel. On the
  // 1 bpp path this is a threshold at the half point, so the same call sites
  // keep working with the format switched off.
  void drawPixelLevel(int x, int y, uint8_t level) const;
  // Write one explicit level at PHYSICAL panel coordinates: no orientation
  // transform, no logical clip rect — only the panel-bounds and strip-band
  // guards, matching FreeInkDisplay::blitImage(). drawImage's 4 bpp branch uses
  // it to land exactly where the 1 bpp blit does; drawPixelLevel() is the
  // logical-coordinate wrapper that rotates once and then calls this.
  void commitPixelPhysical(int phyX, int phyY, uint8_t level) const;
  // True when pixels are 4 bpp nibbles (CP_RENDER_4BPP and not a 1 bpp
  // grayscale-plane strip target). Raw writers key their addressing off this.
  static constexpr bool fourBitCanvas() { return pixfmt::kFourBit; }

  // GRAY16 — the sleep screen's opt-in for 16-level bitmap drawing. It is a
  // one-paint flag, not a mode: set it, draw the bitmaps, clear it. It is
  // deliberately NOT a setting, because the thing it changes is which waveform a
  // FRAME needs, and only the sleep screen is willing to pay that drive.
  void setSixteenLevelImages(bool on) const { imageLevels16 = on; }
  bool sixteenLevelImages() const { return imageLevels16; }

  // Does the composed canvas carry an optical level between the binary endpoints
  // (a nibble other than 0x0/0xF) inside this LOGICAL rectangle? Same test as the
  // panel shim's row_has_grey (metalio_vendor_panel.cpp), but scoped to a region
  // and evaluated BEFORE the drive so the drive MODE can follow the PICTURE.
  //
  // The region is what matters, not the frame: the reader's AA text writes real
  // 0x5/0xA nibbles, so a whole-frame test would answer "yes" on every page that
  // has anti-aliased text and the routing decision would degenerate into "is this
  // an image page" again. Scoped to the image rects the answer is about the image.
  bool regionCarriesTone(int x, int y, int w, int h) const;
  // Draw glyph ink with clipping and orientation resolved once per glyph.
  void drawGlyphBitmap(const uint8_t* bitmap, int width, int height, const glyphBitmap::Frame& frame, bool twoBit,
                       RenderMode mode, bool state) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  bool drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  bool whiteAsTransparent = false) const;
  bool drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  // Counter-invert content images in the logical framebuffer so output-level
  // dark mode leaves their original polarity unchanged.
  void preserveImagePolarity(int x, int y, int width, int height) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Snapshot / restore a screen-coordinate framebuffer region (byte-aligned in
  // panel memory). readFramebufferRegion returns the bytes written to dst, or
  // 0 when the region is empty, offscreen, or exceeds dstCapacity. Pass the
  // same rectangle to writeFramebufferRegion to restore the saved pixels.
  // Enables partial-repaint patterns (e.g. moving a selection highlight)
  // without re-rendering the whole page.
  size_t readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const;
  void writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src);

  // Text
  // Layout may use advance-only SD font tables; rendered measurement includes kerning and ligatures.
  enum class TextMeasureMode { Layout, Rendered };
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const;
  // Draws one glyph at scale256/256 of its size with its baseline at baselineY (drop caps).
  // Outlines are bilinearly resampled from the font bitmap, so no larger font is loaded.
  // Returns the scaled advance in pixels.
  int drawScaledCodepoint(int fontId, uint32_t cp, EpdFontFamily::Style style, int x, int baselineY,
                          int scale256) const;
  // Unscaled metrics for laying out drawScaledCodepoint: advance (12.4 fixed point) and top bearing.
  bool getCodepointMetrics(int fontId, uint32_t cp, EpdFontFamily::Style style, int32_t& advanceFP, int& top) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns kerning plus optional tracking between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style, int8_t tracking = 0) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode mode = TextMeasureMode::Layout) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  // Explicit line spacing scales font size; the one-argument form returns font metrics.
  int getLineHeight(int fontId, float compression) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(RenderMode mode);
  RenderMode getRenderMode() const { return renderMode; }
  // r161 MENU AA: glyph antialiasing for this renderer instance. Set by the
  // activity manager per paint (and by the reader for its own text), never
  // persisted here: see CrossPointSettings::menuAntiAliasing / textAntiAliasing.
  void setTextBinary(const bool binary) const { textBinary_ = binary; }
  bool textBinary() const { return textBinary_; }

  // r163 D3 -- the content-role tag on the drawing path. `setContentRole` is a SETTER,
  // not a push/pop stack, because the tag describes what is being drawn RIGHT NOW and a
  // stale tag is a bug either way; ScopedContentRole below is the only thing call sites
  // should use, so the previous role is restored on every path out.
  // Out of line (GfxRenderer.cpp) so applying a role can log it by name: a denied level
  // looks exactly like a binary pixel on the glass, so the reason has to reach the log.
  void setContentRole(ContentRole role) const;
  ContentRole contentRole() const { return contentRole_; }

  // What the blit actually does: grey is admitted only when the paint's intent allows
  // it AND the content's role allows it. One function, so the three writers (glyph
  // target, explicit-level choke point, scaled codepoint) cannot disagree.
  bool greyscaleAdmitted() const { return !textBinary_ && roleAdmitsGrey(contentRole_); }

  // RAII tag: `const GfxRenderer::ScopedContentRole role(*this, ContentRole::Icon);`
  // restores the previous role when the block ends. Copyable-free on purpose.
  class ScopedContentRole {
   public:
    ScopedContentRole(const GfxRenderer& r, const ContentRole role) : r_(r), previous_(r.contentRole()) {
      r_.setContentRole(role);
    }
    ~ScopedContentRole() { r_.setContentRole(previous_); }
    ScopedContentRole(const ScopedContentRole&) = delete;
    ScopedContentRole& operator=(const ScopedContentRole&) = delete;

   private:
    const GfxRenderer& r_;
    ContentRole previous_;
  };
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  bool displayGrayscaleBase(HalDisplay::GrayscaleMode mode,
                            HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  // Active input encoding, used when drawing monochrome overlays into planes.
  bool grayPlanesAreAbsolute() const { return absoluteGrayPlanes; }

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  // Paper Mono: the base activation is deferred so base + gray planes go out
  // as one waveform. Route the base through displayGrayscaleBase() when true.
  bool combinesGrayscaleBase() const;
  bool storeBwBuffer();  // Returns true if buffer was stored successfully
  // Restore and free the stored buffer. resyncPanelBaseline rewrites the
  // controller's differential baseline to the restored frame — correct after
  // a grayscale render (the glass matches the stored BW plane), WRONG when
  // the glass shows content painted after the store (overlay chrome): the
  // next differential would treat that content as already erased and leave
  // it on the glass. Such callers pass false so the baseline keeps tracking
  // what was last pushed.
  void restoreBwBuffer(bool resyncPanelBaseline = true);
  // Free a stored buffer without restoring it (the page under it changed).
  void discardStoredBwBuffer() { freeBwBufferChunks(); }
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }
  // Bumped by every loan. A loan returns the buffer white, so a caller that ran code which may lend
  // (e.g. a background chapter-build step) compares this to know the page it drew is gone.
  uint32_t frameBufferLoanCount() const { return frameBufferLoans; }

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
