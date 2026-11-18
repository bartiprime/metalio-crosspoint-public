#pragma once

#include <GfxRenderer.h>
#include <fontIds.h>

#include <algorithm>
#include <cstdint>

#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

// r162 item 6 -- the reader's IN-BOOK MENU chrome, in one place, for BOTH of its
// presentations:
//
//   * list mode    -- EpubReaderMenuActivity: the full-screen fui list
//   * toolbar mode -- ReaderToolbarUi: the bottom sheet and its Contents/Text/
//                     More panels
//
// THE RULE. The menu adopts the ACTIVE theme's vocabulary -- its faces, its
// spacing unit, its left/right rail -- but it does NOT inherit the theme's
// browse-list chrome. Those are two different things and the earlier attempts
// failed by conflating them:
//
//   * routing the whole screen at the stock theme (the "make it look like
//     upstream" attempt) is not a restyle, it is a different family;
//   * leaving themeListChrome() on hands this screen the LIST numbers -- the
//     108 px doubled app bar (2 x Andro's 40 design headerHeight), the 6 px band
//     rule, the 92 px settings pitch and the 56 px icon box sampled from the 64 px
//     ROW masks. A reader menu is not a browse list: it has no icons at all, its
//     labels are the app's longest strings, and it is a fixed set of ~14-19
//     controls a reader reaches for in the middle of a page. A stretched
//     browse-list row is the wrong measure for it, and an app bar whose 6 px rule
//     lands directly above the menu's progress line reads as TWO bars.
//
// So this file derives the menu's own numbers from the live theme. Every value
// is measured, not asserted: change the panel density, the theme's ladder or its
// app-bar button and the menu follows. Both presentations call these, so the list
// and the sheet cannot drift apart -- the failure mode the operator named when
// the toolbar mode and the list mode stopped looking related.
namespace readerMenuChrome {

// FreeInkUICore's DeviceContext floor, and the +14 the SDK's own
// themeTokensForLineHeight() adds to a line box for its touch minimum. Both are
// restated here so the derivation does not depend on which face some earlier
// paint happened to bind into FONT_BODY: r101.11 BAND1 is exactly that bug (a
// list rebinds FONT_BODY to its row face, minTouchSize then reads 74 instead of
// 44, and every derived size moves with paint history). The canonical binding is
// uiScaleSpec()'s body face, so it is what we measure.
constexpr int16_t kSdkTouchFloor = 44;
constexpr int16_t kLineBoxTouchPad = 14;

// Clear above and below a menu row label's line box. 8 px at 297 PPI is 0.68 mm:
// enough that two labels never read as one block, small enough that the row
// stays a menu row rather than a settings row.
constexpr int16_t kRowAir = 8;

// Clear above and below the tallest thing in the header band.
constexpr int16_t kBandClear = 4;

// THE touch standard for this screen, and the row PITCH, from one derivation:
// the project rule is that a UI element is 50 % larger than the touch minimum,
// not merely equal to it. On the Plus panel this is ceil(1.5 x 44) = 66 px --
// the same number for the rows, the pager arrows and the sheet's rows.
inline int16_t touchStandard(const GfxRenderer& renderer) {
  const int16_t bodyLine = static_cast<int16_t>(renderer.getLineHeight(uiScaleSpec().bodyFontId));
  const int16_t touchMin = std::max<int16_t>(kSdkTouchFloor, static_cast<int16_t>(bodyLine + kLineBoxTouchPad));
  return static_cast<int16_t>((touchMin * 3 + 1) / 2);  // ceil(1.5 x touchMin)
}

inline int16_t rowPitch(const GfxRenderer& renderer) { return touchStandard(renderer); }

// The row label face: the LARGEST rung of the theme's own ladder whose line box
// still leaves kRowAir above and below inside one row of the pitch. The ladder
// top (Andro's Ember 24, a 60 px line box) cannot sit in a 66 px row -- 3 px of
// air is a block, not a row -- so a menu picks the next rung down that can. It
// stops at the theme's own faces, so this is still the theme speaking; it just
// is not the size it reserves for a browse list, where the row is 92 px.
// Returns 0 when the theme offers no ladder (every theme but Andro), and the
// caller keeps the tokens' body face.
inline int rowFace(const GfxRenderer& renderer) {
  const BaseTheme& theme = UITheme::getInstance().getTheme();
  const int16_t pitch = rowPitch(renderer);
  int count = 0;
  const int* ladder = theme.listRowFontLadder(count);
  if (ladder == nullptr) return 0;
  for (int i = 0; i < count; ++i) {
    if (static_cast<int16_t>(renderer.getLineHeight(ladder[i]) + 2 * kRowAir) <= pitch) return ladder[i];
  }
  return 0;
}

// Row icon box: NONE. Not one row of this menu carries an icon -- buildMenuRowItems()
// fills label + value + actionValue only, and the two switch rows (Bluetooth, and
// the handlers' Night Mode / Frontlight) carry a toggle, which has its own
// geometry. The theme's 56 px box exists to sample the 64 px browse-list row
// masks; reserving it here would be 56 px of dead air in every row.
constexpr int16_t rowIconBox() { return 0; }

// Height of the header band, in panel pixels.
//
// Sized by the two things that physically go in it, from the theme: the face the
// theme titles its bands with (Andro: Ember 24, a 60 px line box) and the leading
// button square the theme asks for on such a band (Andro: 72 px), plus kBandClear
// top and bottom. On the Plus panel: max(60, 72) + 8 = 80 px.
//
// 80 px, not the 108 px app bar: the app bar is a browse-list affordance. The
// menu has no tree to back out of and its content is the point, so the band is
// 74 % of the app bar's height and the menu gets the difference back as rows.
// It is still taller than Andro's 54 px home status band because a 60 px title
// face has to fit in it.
//
// A theme that asks for neither a titled face nor a titled button -- every theme
// but Andro -- keeps its own band height, so nothing else moves.
inline int16_t bandHeight(const GfxRenderer& renderer) {
  const BaseTheme& theme = UITheme::getInstance().getTheme();
  const int face = theme.titledHeaderFontId();
  const int button = theme.titledHeaderButtonSize();
  if (face == 0 && button == 0) return static_cast<int16_t>(UITheme::getInstance().getMetrics().headerHeight);
  const int16_t titleLine =
      static_cast<int16_t>(renderer.getLineHeight(face != 0 ? face : uiScaleSpec().titleFontId));
  const int16_t buttonSquare = static_cast<int16_t>(button > 0 ? button : 48);
  return static_cast<int16_t>(std::max(titleLine, buttonSquare) + 2 * kBandClear);
}

// Face for the band's title: the theme's titled face when it has one (coverage
// checked by the caller -- Ember has no fallback, so a non-Latin title must keep
// the UI face rather than print tofu), else the UI title face.
inline int titleFace() {
  const int face = UITheme::getInstance().getTheme().titledHeaderFontId();
  return face != 0 ? face : uiScaleSpec().titleFontId;
}

// Caption face for the progress line under the band: the theme's own caption
// face (Andro: Ember 12, the same run of text as its status bar), else the UI
// small face. The line reads as a caption, not as a second band, which is why it
// is sized to its line box instead of taking a tabBarHeight strip.
inline int captionFace() {
  const int face = UITheme::getInstance().getTheme().captionFontId();
  return face != 0 ? face : uiScaleSpec().smallFontId;
}

}  // namespace readerMenuChrome
