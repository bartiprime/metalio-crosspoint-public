#pragma once

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <cstdint>

#include "components/HeaderBackTapTarget.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/headerIcons.h"
#include "components/icons/headerIcons56.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// ONE implementation of the header band every titled screen paints, extracted
// from BaseTheme::drawHeader so a screen that composes its OWN HeaderProps (its
// own leading/trailing actions -- the library list, the catalog family) gets the
// same band instead of a hand-rolled second recipe: the bezel fitting, the
// doubled titled-band geometry (leading square, band-centred title), the
// coverage-guarded theme title face and the theme's rule.
//
// The helper owns everything the theme derives from the band and from the
// tokens: title/rightLabel, the battery+clock cluster (or its removal on a theme
// without one), titleText/subtitleText/styles/sidePadding/borderEdges, the
// leading back button's geometry and HeaderBackTapTarget. The CALLER owns what
// makes its header its own: trailing icon/action pairs, leadingAction when it
// routes the arrow itself, minTouchSize, trailing styles.
//
// `backButton` = false means "the caller's leadingAction routes the tap": the
// helper still sizes and masks the arrow, but it does not register
// HeaderBackTapTarget (it clears it, so a stale rect from another screen cannot
// fire here).
namespace titledHeaderBand {

// Departures from the theme's titled band that a SCREEN may legitimately ask for,
// so it does not have to re-implement this recipe (and cannot grow a second one).
// Defaults keep every existing caller on the theme's own band, pixel for pixel.
struct BandOptions {
  // Paint the theme's own rule under the band (drawTitledBandRule -- Andro's 6 px
  // app-bar stroke). FALSE means "this screen has no divider under its band":
  // RDR1, the reader's band, where the operator removed that line. The other two
  // edge mechanisms are unaffected (see the comment at the call site below).
  bool paintThemeRule = true;
  // Band height in panel pixels, or 0 for theme.titledHeaderPx(). A screen whose
  // band is deliberately SHORTER than the theme's app bar (the reader menu) names
  // its own; anything at least this tall still gets the theme's titled chrome
  // (title face, leading button, band-centred title).
  int16_t bandHeightPx = 0;
  // tokens.headerSidePadding override, or -1. A screen that aligns its title with
  // its own content rail passes that rail instead.
  int16_t sidePadding = -1;
};

template <size_t N>
void drawTitledHeaderBand(const BaseTheme& theme, freeink::ui::Frame<N>& frame, const GfxRenderer& renderer,
                          Rect rect, const char* title, const char* subtitle, freeink::ui::HeaderProps& props,
                          const bool backButton, const BandOptions& options = BandOptions{}) {
  namespace fui = freeink::ui;
  // Scaled metrics for the band: the titled-height decision below compares
  // against the home band, so both numbers come from the same table.
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // The frame's target IS the board's GfxRendererTarget on every screen in this
  // firmware (GfxRendererFrame::target / UiAppHost::uiTarget). The font SLOTS can
  // only be rebound through it -- DrawTarget has no setFont -- so this is the one
  // downcast the band needs, stated once instead of in every caller.
  auto& target = static_cast<fui::GfxRendererTarget&>(frame.target());
  // Refresh the app-wide shared tokens instead of copying ~1.5KB of ThemeTokens
  // onto this render-path stack frame; the values derived here are identical to
  // what every FreeInkApp screen derives. Goes through the same
  // publish-a-fresh-slot path applySharedUiTheme() uses (see UiAppHelpers.h)
  // rather than overwriting the previously-published instance in place, since
  // some other FreeInkApp could be mid-read of it.
  //
  // r101.11 BAND1: REBUILD FROM THE CANONICAL UI BINDING, NEVER THIS FRAME'S.
  // uiThemeTokens() derives rowHeight/headerHeight/minTouchSize (body line height
  // + 14) from whatever face FONT_BODY happens to hold. A list screen rebinds that
  // slot to the theme's ROW face to draw its rows (UiListActivity::
  // applyThemeListChrome -> fitThemeRowLabels), and the slot keeps that binding
  // for the life of the activity -- so from the SECOND paint of a list screen on,
  // this call measured Ember 24 (line height 60) instead of the UI body face
  // (<= 30) and republished minTouchSize = 74 where the first paint published 44.
  // The library tab band reads it as ceil(1.5 x 74) + 6 = 117 px after a tab
  // switch against 72 px on the cold open: one theme metric, two values, chosen by
  // paint history. makeUiTarget() binds the uiScale faces (FONT_BODY = UI_12) --
  // the binding these tokens are DEFINED against, and the one this band and
  // OptionPopup both refresh through.
  const fui::ThemeTokens& tokens = refreshSharedUiThemeTokens(makeUiTarget(renderer));
  // Header status text (battery percent, right label) stays at the fixed small
  // font like the legacy headers; the uiScale small font is for list subtitles.
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  target.setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);

  props.title = title;
  props.rightLabel = subtitle;  // firmware headers right-align the secondary text
  // Battery + clock chrome and their title reserves live in the FreeInkUI
  // header component; this only fills the values from settings and metrics.
  BaseTheme::applyHeaderStatus(renderer, props);
  if (!theme.headerShowsStatusCluster()) {
    // Titled bands on a theme without the status line (Andro: the Android app
    // bar). Clear what the shared chrome just filled -- battery glyph, percent,
    // clock, their strip height and the title reserves they imply -- and put the
    // title and the back button on the band's OWN centre line, since the strip
    // they were nudged below is not painted. Nothing here touches other themes:
    // the virtual is true everywhere else, and Andro's home band never reaches
    // this call (AndroTheme paints its own cluster there).
    props.status = fui::HeaderStatusProps{};
    props.titleOffsetY = 0;
    props.actionOffsetY = 0;
  }
  // Callers pass a full-width header rect; headerBand fits it to the board's bezel.
  const fui::Rect band = BaseTheme::headerBand(
      renderer,
      fui::Rect{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y), static_cast<int16_t>(rect.width),
                static_cast<int16_t>(rect.height)},
      props);
  if (rect.height < metrics.headerHeight) {
    // Short bands (home, a status strip) are not split into strip + content row:
    // the title centers on the band, clear of the band's bottom edge.
    props.titleOffsetY = 0;
  }
  // --- doubled titled band (a theme's app bar) ------------------------------
  // A theme may ask for a TITLED band taller than the home band (Andro: 2x).
  // Any caller that passes at least that height gets chrome scaled to it: the
  // title face, the leading arrow and its square grow with the band, and both
  // sit on the band's own centre line. It applies only to a theme that also
  // drops the status cluster -- with a strip painted at the top there would be
  // nothing to centre the larger title against.
  const int titledH = options.bandHeightPx > 0 ? options.bandHeightPx : theme.titledHeaderPx();
  const bool tallTitled = title != nullptr && !theme.headerShowsStatusCluster() && titledH > metrics.headerHeight &&
                          rect.height >= titledH;
  if (tallTitled) {
    const int buttonSize = theme.titledHeaderButtonSize();
    if (buttonSize > 0) {
      props.leadingSize = static_cast<int16_t>(buttonSize);
      props.trailingSize = static_cast<int16_t>(buttonSize);
    }
    // Band-centred box: rect.y + 4 + actionOffsetY is the band's midline minus
    // half the button. The title needs no offset -- fui::header centres its line
    // block in the band, and the strip that used to lower it is cleared above.
    props.actionOffsetY = static_cast<int16_t>((rect.height - props.leadingSize) / 2 - 4);
  }
  // Back button leading the band on touch boards, so every pushed screen offers
  // a visible way out beside the edge-swipe gesture. This frame registers no hit
  // rects of its own here, so the rect is recorded in HeaderBackTapTarget and
  // MappedInputManager folds taps on it into Button::Back. Same geometry as the
  // FUI header's leading slot (applyHeaderStatus set the size/offset) so the
  // recorded rect matches the drawn button.
  const int16_t backBtnSize = props.leadingSize;
  const bool callerRoutesLeading = props.leadingAction != fui::NO_ACTION;
  const bool showBackButton = (backButton || callerRoutesLeading) && title != nullptr && gpio.hasTouch();
  if (showBackButton) {
    // The tall band's arrow comes from the mask authored at 56 px: growing an
    // icon past its source resolution is a resampling problem, and the 32 px
    // shared mask upscaled to 56 aliases badly on a 1-bit panel.
    if (!props.leadingIcon) {
      props.leadingIcon = fui::bitmapFromIcon(tallTitled && props.leadingSize >= 56 ? icon_header_back_56
                                                                                    : icon_header_back_32);
    }
    if (!callerRoutesLeading) props.leadingAction = 1;  // any non-NO_ACTION id paints the button
    if (backButton) {
      HeaderBackTapTarget::set(band.x + 4, band.y + 4 + props.actionOffsetY, backBtnSize, backBtnSize);
    } else {
      // The caller's own leading action owns this tap; a rect recorded here
      // would fire Button::Back as well as the caller's handler.
      HeaderBackTapTarget::clear();
    }
  } else {
    HeaderBackTapTarget::clear();
  }
  // ONE stroke on the band's bottom edge.
  //
  // Three mechanisms can put ink on that edge and only one may be live:
  //   1. the FreeInkUI frame's own bottom border -- drawn by freeink::ui::header()
  //      when the resolved style carries a border AND a width. Dead for Andro
  //      today only because tokens.popup comes from defaultPopupStyles(), which
  //      ships no border; props.borderEdges alone paints nothing without one;
  //   2. the header-underline border installed just below when
  //      tokens.headerUnderline > 0 (dead for Andro: headerUnderlineSize is 0);
  //   3. the theme's own rule, drawTitledBandRule() -- Andro's 6 px stroke.
  // A theme that paints its own rule owns the edge, so both frame borders are
  // SUPPRESSED here instead of being switched off by a zero metric: on that edge
  // there is exactly one drawing call, and it is the theme's. Every other theme
  // (paintsOwnBandRule() == false) keeps the frame border it has always used.
  const bool themePaintsRule = theme.paintsOwnBandRule();
  props.borderEdges = themePaintsRule ? static_cast<uint8_t>(fui::EdgesNone)
                                      : static_cast<uint8_t>(fui::EdgeBottom);
  props.titleText = tokens.titleText;
  props.titleText.align = tokens.headerTitleAlign;
  if (tallTitled) {
    // BIND THE SLOT, do not write a font id into titleText.font: TextStyle::font is
    // a SLOT index (FONT_SMALL/BODY/TITLE), and anything past them resolves back to
    // FONT_BODY -- the same trap that kept r73's row font off the glass. The frame
    // was built with FONT_TITLE = uiScaleSpec().titleFontId, and props.titleText came
    // from the theme tokens carrying that slot, so rebinding the slot swaps the face
    // for this one band. Same glyph-coverage guard the settings rows use: a title in
    // a script Ember does not carry keeps UI_12 instead of printing holes.
    const int face = theme.titledHeaderFontId();
    if (face != 0 && BaseTheme::textFitsFace(renderer, face, title)) {
      target.setFont(fui::GfxRendererTarget::FONT_TITLE, face);
    }
  }
  props.subtitleText = tokens.smallText;
  props.styles = tokens.popup;
  props.sidePadding = options.sidePadding >= 0 ? options.sidePadding : tokens.headerSidePadding;
  // Underline only under a titled header: an untitled band (Lyra home screen)
  // historically drew no rule, and the old themes keyed the line on the title.
  // Skipped for a theme that paints its own rule: this would be a SECOND stroke
  // on the same edge, at a different weight (Design-scaled, so 6 -> 8 px).
  if (!themePaintsRule && title != nullptr && props.styles.normal.border.kind == fui::PaintKind::None &&
      tokens.headerUnderline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = tokens.headerUnderline;
  }
  freeink::ui::header(frame, band, props);
  // BATT6: the percentage NUMBER is drawn INSIDE the SDK's battery component, so this
  // path has no box it could repaint in place -- and re-deriving the component's
  // reserve arithmetic here is exactly the drift the record exists to avoid. What is
  // recorded instead is the VALUE the header just painted: the change producer then
  // compares the gauge against it and, on a real change, asks for ONE whole-activity
  // repaint (main.cpp pins it to a diff-masked DU, so the panel still drives only the
  // pixels that differ -- no clean). That covers every titled screen in the firmware:
  // Home, the library, settings, the catalog family.
  if (props.status.showBattery && props.status.battery.label != nullptr) {
    renderer.noteBatteryPercentUnaddressable(props.status.battery.percent);
  }
  // The theme's rule under its own band (Andro's 6 px app-bar rule). A hook
  // rather than code inside drawHeader, so a screen that composes its header
  // through this helper carries the same ink and no second rule is drawn. A
  // screen with no divider under its band (RDR1: the reader) opts out here, and
  // then the band's bottom edge carries no ink at all.
  if (options.paintThemeRule) theme.drawTitledBandRule(renderer, rect);
}

}  // namespace titledHeaderBand