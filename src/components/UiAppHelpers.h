#pragma once
#include <EpdFontFamily.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>
#include <Utf8.h>

#include <atomic>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/listIcons64.h"

// Shared glue for activities hosting a FreeInkApp: the font-bound render
// target and the touch snapshot FreeInkApp routing consumes.

// One app-wide ThemeTokens instance shared by every FreeInkApp via
// setThemeRef, so per-app copies (~1.5KB each, and one per stacked activity)
// aren't pure heap waste. Refreshed on every screen entry, so theme or font
// changes between activities re-derive it; live theme changes (Settings)
// refresh it and every referencing app repaints in the new look.
//
// Backed by a small pool + an atomic cell (FreeInkApp::setThemeRef() takes a
// pointer to the cell, not to a ThemeTokens instance directly) rather than a
// single instance overwritten in place: refreshSharedUiThemeTokens() below
// always builds the new tokens into whichever pool slot the cell does NOT
// currently reference, then does one atomic store. Every app sharing the
// cell picks up the change on its next theme() call, and nothing ever
// dereferences an instance mid-overwrite — unlike a plain
// `sharedTokens = uiThemeTokens(target);` in-place assignment, which a
// render task reading theme().rowHeight/etc. field-by-field on another task
// could observe as a torn mix of old and new fields.
inline std::atomic<const freeink::ui::ThemeTokens*>& sharedUiThemeCell() {
  static std::atomic<const freeink::ui::ThemeTokens*> cell{nullptr};
  return cell;
}

// Rebuilds the shared tokens for `target` and atomically publishes them via
// sharedUiThemeCell(). Returns the freshly-published instance for callers
// that also want to read it back immediately (e.g. BaseTheme::drawHeader(),
// which derives the same tokens as a render-path scratch value instead of
// stack-allocating its own copy).
inline const freeink::ui::ThemeTokens& refreshSharedUiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  static freeink::ui::ThemeTokens pool[2];
  auto& cell = sharedUiThemeCell();
  const auto* current = cell.load(std::memory_order_relaxed);
  freeink::ui::ThemeTokens* next = (current == &pool[0]) ? &pool[1] : &pool[0];
  *next = uiThemeTokens(target);
  cell.store(next, std::memory_order_release);
  return *next;
}

// Refresh the shared tokens from the active UITheme + this target's fonts and
// point the app at them. Replaces the old per-app `app.setTheme(...)` copies.
template <typename App>
inline void applySharedUiTheme(App& app, const freeink::ui::GfxRendererTarget& target) {
  refreshSharedUiThemeTokens(target);
  app.setThemeRef(&sharedUiThemeCell());
}

// Bind the uiScale fonts before FreeInkApp's constructor derives its theme
// metrics from the body font's line height.
inline freeink::ui::GfxRendererTarget makeUiTarget(const GfxRenderer& renderer) {
  freeink::ui::GfxRendererTarget target(renderer, BoardConfig::hasTouch());
  const auto spec = uiScaleSpec();
  target.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, spec.titleFontId);
  // Status chrome (header battery percent, clock) stays at the fixed small
  // font; the uiScale FONT_SMALL is for list subtitles.
  target.setFont(freeink::ui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
  return target;
}
// True when the face a themed list row would be drawn with can draw every
// codepoint of `label`. The baked Ember faces carry Latin-1, Latin Extended-A/B
// and Cyrillic plus punctuation and currency, but NO non-Latin fallback is
// registered for the Ember ids (SdCardFontSystem maps only SMALL/UI_10/UI_12),
// so a Hebrew, Arabic, Greek, CJK or Thai label drawn in Ember would print with
// holes. Asking the LOADED face instead of a hardcoded script list keeps this
// true if the baked fonts ever change.
//
// ONE rule, every caller: UiListActivity::themeRowLabelsFit() scans a page of
// rows through it before it binds the theme's row face, and the screens that
// build their own list (Settings > Controls > Button remap) ask it about their
// own handful of labels. A second copy of this test is how a sub-menu ends up
// either printing holes or silently falling back to the UI face on one screen
// and not the others.
// Whether a face is present in the loaded font map at all. Every font id the UI
// can select must answer here before anything binds or measures it: a rung the
// binary never baked is a visual fallback (keep the next face), never a
// dereference of a missing family.
inline bool uiFaceLoaded(const GfxRenderer& renderer, const int fontId) {
  if (fontId == 0) return false;
  const auto& faces = renderer.getFontMap();
  return faces.find(fontId) != faces.end();
}

inline bool uiLabelFitsFace(const GfxRenderer& renderer, const int fontId, const char* label) {
  if (label == nullptr) return true;
  if (!uiFaceLoaded(renderer, fontId)) return false;  // face not loaded: keep the theme's font
  const EpdFontFamily& face = renderer.getFontMap().find(fontId)->second;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(label);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&cursor)) != 0) {
    if (cp >= 0x80 && !face.hasCodepoint(cp, EpdFontFamily::REGULAR)) return false;
  }
  return true;
}

// ANDRO4: the theme's row chrome for a HAND-BUILT list.
//
// Most list screens derive from UiListActivity and reach
// applyThemeListChrome(); a few build their own ListProps (Settings > Controls >
// Button remap, the CrossPoint Sync result rows, the Wi-Fi network list). Without
// this they drew in the UI body face at the SDK's default pitch while every page
// above them wore the theme's row face -- the divergence ANDRO4 swept.
//
// Two knobs, and both are no-ops for a theme that does not override them:
//   * the row face, bound into FONT_BODY (the slot list labels resolve through)
//     and only when it carries EVERY string that draws through that slot -- the
//     Ember ids have no registered fallback, so a label in another script keeps
//     the UI face instead of printing holes. `bodyText` lets a caller add the
//     non-row strings it also draws through FONT_BODY (a section label), so the
//     slot is bound ONCE and every string sized through it is measured with the
//     face it will be drawn in.
//   * the row pitch from listRowFillPitch(), written into ListProps::rowHeight
//     with rowGap 0, so the painted row, its tap band and the page size are one
//     number.
// Returns the pitch it applied (0 when the theme supplies neither knob, or when
// the face could not carry the strings).
inline int16_t applyThemeRowChrome(const GfxRenderer& renderer, freeink::ui::GfxRendererTarget& target,
                                   const BaseTheme& theme, const freeink::ui::ListItem* items, const int count,
                                   freeink::ui::ListProps& props, const int bandHeight,
                                   const char* const* bodyText = nullptr, const int bodyTextCount = 0) {
  const int rowFontId = theme.listRowFontId();
  if (!theme.themeListChrome() || rowFontId == 0) return 0;
  bool fits = true;
  for (int i = 0; i < count && fits; ++i) fits = uiLabelFitsFace(renderer, rowFontId, items[i].label);
  for (int i = 0; i < bodyTextCount && fits; ++i) fits = uiLabelFitsFace(renderer, rowFontId, bodyText[i]);
  // Rebind on BOTH outcomes: the slot keeps whatever face the last build left in
  // it, so falling back without rebinding would leak the theme face to the next
  // screen that shares this target.
  props.labelText.font = freeink::ui::GfxRendererTarget::FONT_BODY;
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, fits ? rowFontId : uiScaleSpec().bodyFontId);
  if (!fits) return 0;
  // The theme's row ICON box is deliberately NOT applied here: these hand-built
  // rows carry the 24 px FreeInkUI masks (download/upload, the Wi-Fi signal bars),
  // and there is no 64 px counterpart to sample down from -- growing the box would
  // upscale 1-bpp art, the exact raggedness r78 removed from the settings rows.
  // Reported as a structural gap rather than papered over.
  const int pitch = theme.listRowFillPitch(bandHeight, count);
  if (pitch <= 0) return 0;
  props.rowHeight = static_cast<int16_t>(pitch);
  props.rowGap = 0;
  return static_cast<int16_t>(pitch);
}

// Tap release with coords, plus the raw release the tap classifier never
// reports (swipe end, drag-off) delivered off-target: nothing dispatches,
// but routing drops its pressed-element state instead of ghosting it onto
// the next render.
// Firmware UIIcon -> FreeInkUI bitmap for list rows (SDK-format icons only;
// the legacy drawIcon assets use a different bit layout). Two crisp sizes:
// 24 for single-line rows, 32 for label+subtitle rows.
inline freeink::ui::BitmapRef listIconFor(const UIIcon icon, const int size = 24) {
  // r78: a theme whose row icon box is BIGGER than the largest authored 32 px
  // mask (Andro: 56 px) samples the 64 px masks instead of upscaling 32 px art
  // -- an upscale on a 1-bpp panel is what made these rows look ragged. Every
  // other theme returns 0 here and keeps the 24/32 px sets byte-for-byte.
  if (UITheme::getInstance().getTheme().listRowIconSize() > 32) {
    switch (icon) {
      case UIIcon::Folder:
        return freeink::ui::bitmapFromIcon(icon_list_folder_64);
      case UIIcon::Text:
        return freeink::ui::bitmapFromIcon(icon_list_file_text_64);
      case UIIcon::Image:
        return freeink::ui::bitmapFromIcon(icon_list_image_64);
      case UIIcon::Book:
        return freeink::ui::bitmapFromIcon(icon_list_book_64);
      case UIIcon::File:
        return freeink::ui::bitmapFromIcon(icon_list_file_64);
      case UIIcon::Wifi:
        return freeink::ui::bitmapFromIcon(icon_list_wifi_64);
      case UIIcon::Library:
        return freeink::ui::bitmapFromIcon(icon_list_library_64);
      case UIIcon::Plugins:
        return freeink::ui::bitmapFromIcon(icon_list_blocks_64);
      case UIIcon::Hotspot:
        return freeink::ui::bitmapFromIcon(icon_list_radio_tower_64);
      case UIIcon::Usb:
        return freeink::ui::bitmapFromIcon(icon_list_usb_64);
      case UIIcon::Bookmark:
        return freeink::ui::bitmapFromIcon(icon_list_bookmark_64);
      case UIIcon::Blocks:
        return freeink::ui::bitmapFromIcon(icon_list_blocks_64);
      default:
        return {};
    }
  }
  if (size >= 32) {
    switch (icon) {
      case UIIcon::Folder:
        return freeink::ui::bitmapFromIcon(icon_folder_32);
      case UIIcon::Text:
        return freeink::ui::bitmapFromIcon(icon_file_text_32);
      case UIIcon::Image:
        return freeink::ui::bitmapFromIcon(icon_image_32);
      case UIIcon::Book:
        return freeink::ui::bitmapFromIcon(icon_book_32);
      case UIIcon::File:
        return freeink::ui::bitmapFromIcon(icon_file_32);
      case UIIcon::Wifi:
        return freeink::ui::bitmapFromIcon(icon_wifi_32);
      case UIIcon::Library:
        return freeink::ui::bitmapFromIcon(icon_library_32);
      case UIIcon::Plugins:
        return freeink::ui::bitmapFromIcon(icon_blocks_32);
      case UIIcon::Hotspot:
        return freeink::ui::bitmapFromIcon(icon_radio_tower_32);
      case UIIcon::Usb:
        return freeink::ui::bitmapFromIcon(icon_usb_32);
      case UIIcon::Bookmark:
        return freeink::ui::bitmapFromIcon(icon_bookmark_32);
      case UIIcon::Blocks:
        return freeink::ui::bitmapFromIcon(icon_blocks_32);
      default:
        return {};
    }
  }
  switch (icon) {
    case UIIcon::Folder:
      return freeink::ui::bitmapFromIcon(icon_folder_24);
    case UIIcon::Text:
      return freeink::ui::bitmapFromIcon(icon_file_text_24);
    case UIIcon::Image:
      return freeink::ui::bitmapFromIcon(icon_image_24);
    case UIIcon::Book:
      return freeink::ui::bitmapFromIcon(icon_book_24);
    case UIIcon::File:
      return freeink::ui::bitmapFromIcon(icon_file_24);
    case UIIcon::Wifi:
      return freeink::ui::bitmapFromIcon(icon_wifi_24);
    case UIIcon::Library:
      return freeink::ui::bitmapFromIcon(icon_library_24);
    case UIIcon::Plugins:
      return freeink::ui::bitmapFromIcon(icon_blocks_24);
    case UIIcon::Hotspot:
      return freeink::ui::bitmapFromIcon(icon_radio_tower_24);
    case UIIcon::Usb:
      return freeink::ui::bitmapFromIcon(icon_usb_24);
    case UIIcon::Bookmark:
      return freeink::ui::bitmapFromIcon(icon_bookmark_24);
    case UIIcon::Blocks:
      return freeink::ui::bitmapFromIcon(icon_blocks_24);
    default:
      return {};
  }
}

// Bottom-anchored Cancel / OK pair for slider dialogs on touch devices, where
// the physical Back/Confirm buttons (and their auto-hidden hints) may not
// exist. Callers gate on hasTouch(): button boards keep the hint chrome and
// need no on-screen pair. Consumes the bottom of the screen's content band.
template <typename Screen>
inline void addDialogCancelOk(Screen& screen, const freeink::ui::ActionId cancelAction,
                              const freeink::ui::ActionId okAction) {
  const auto& theme = screen.theme();
  const int16_t sideInset = static_cast<int16_t>(theme.spaceLg * 2);
  const freeink::ui::Rect band =
      screen.takeBottom(theme.rowHeight, theme.spaceLg).inset(freeink::ui::Insets{0, sideInset, 0, sideInset});
  const int16_t gap = theme.spaceLg;
  const int16_t buttonWidth = static_cast<int16_t>((band.width - gap) / 2);

  freeink::ui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = cancelAction;
  cancel.inputMask = freeink::ui::InputTouch;
  cancel.text = theme.bodyText;
  freeink::ui::ButtonProps ok = cancel;
  ok.label = tr(STR_OK_BUTTON);
  ok.action = okAction;
  freeink::ui::button(screen.frame(), freeink::ui::Rect{band.x, band.y, buttonWidth, band.height}, cancel);
  freeink::ui::button(
      screen.frame(),
      freeink::ui::Rect{static_cast<int16_t>(band.x + band.width - buttonWidth), band.y, buttonWidth, band.height}, ok);
}

// withLongPress: the SDK touch classifier fires the long-press WHILE the
// finger is still down (matching the physical-button hold-to-act feel) and
// suppresses the remainder of the contact, so the finger lift can't also
// tap-dismiss the popup the long-press opens. Delivered as a touchReleased +
// longPress snapshot at the contact point; only rows masked InputLongPress
// receive it. Mirrors the SDK's long-press-aware fui::snapshotFrom, but maps
// coordinates through the renderer's LIVE orientation (the reader rotates at
// runtime), which the DeviceContext-based SDK adapter does not track.
inline freeink::ui::InputSnapshot touchSnapshotFrom(const MappedInputManager& mappedInput,
                                                    const bool withLongPress = false) {
  int tx = 0;
  int ty = 0;
  if (withLongPress && mappedInput.wasScreenLongPress(tx, ty)) {
    freeink::ui::InputSnapshot snap{};
    snap.touchReleased = true;
    snap.longPress = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
    return snap;
  }

  freeink::ui::InputSnapshot snap{};
  // Live contact position: only InputDrag-masked elements (sliders) react, so
  // carrying it in every snapshot is free for ordinary screens.
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    snap.touchHeld = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    snap.touchPressed = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    snap.touchReleased = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  } else if (mappedInput.wasScreenTouchReleased()) {
    snap.touchReleased = true;
    snap.touchX = -1;
    snap.touchY = -1;
  }
  return snap;
}
