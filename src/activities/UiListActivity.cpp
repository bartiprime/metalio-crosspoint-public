#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/InPlaceValueStep.h"
#include "audio/UiSounds.h"  // AUDIO1: select / back / move / refused feedback
#include "components/UIScale.h"
#include "display/EqualizeBlack.h"
#include "platform/PlatformFeatures.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// FNV-1a over a row's visible strings. Cheap enough to run per built page (a
// build already measures and wraps every row) and strong enough that the
// buffer-flip skip in render() cannot mistake a changed row for an unchanged
// one -- the alternative, trusting "top did not move", would swallow a data
// reload that reuses the same viewport.
uint32_t hashRowText(uint32_t h, const char* text) {
  if (!text) return (h ^ 0x9E3779B9u) * 16777619u;
  while (*text) h = (h ^ static_cast<uint8_t>(*text++)) * 16777619u;
  return h;
}

// One chevron arm from (x0,y0) to (x1,y1) at `ink` px. A live arm is a single
// heavy line; a dimmed (disabled) arm has to be built from 1 px fill blocks,
// because on this target both DrawTarget::line() and DrawTarget::bitmap()
// collapse a dithered paint to solid ink ("color != White" is a bool there).
// fill() and the radius-0 rect stroke() are the only primitives that carry the
// dither through -- and a disabled button that prints as black is not disabled.
void chevronArm(fui::DrawTarget& target, const int x0, const int y0, const int x1, const int y1, const int ink,
                const fui::Paint paint, const bool solid) {
  const int dy = y1 - y0;
  const int dx = x1 - x0;
  if (solid || dy == 0) {
    target.line(fui::Point{static_cast<int16_t>(x0), static_cast<int16_t>(y0)},
                fui::Point{static_cast<int16_t>(x1), static_cast<int16_t>(y1)}, static_cast<uint8_t>(ink), paint);
    return;
  }
  const int steps = dy < 0 ? -dy : dy;
  const int dir = dy < 0 ? -1 : 1;
  for (int i = 0; i <= steps; ++i) {
    const int x = x0 + dx * i / steps;
    const int y = y0 + dir * i;
    target.fill(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(ink), 1}, paint);
  }
}

// The pager readout's side air, in panel pixels. The digits are right-aligned
// into the band inset by this much, so they never touch the panel edge.
constexpr int kReadoutPad = 6;

// THE only way to read row `index` out of a ListProps. `index` is a GLOBAL row
// index -- the same index space as ListProps::count, ListNav::top and the row
// action values -- NOT an offset into props.items.
//
// The bug this exists to prevent is not hypothetical. A windowed list (Library,
// the reader's TOC and clippings, the reader toolbar) sets props.count to the
// TOTAL row count while props.items points at a PAGE-SIZED window described by
// itemsWindowFirst/itemsWindowCount. Indexing props.items[i] with a global i
// then reads far past that window's allocation -- a heap read of whatever lives
// next, dereferenced as a `const char*` label, i.e. an exception on entry with
// no core dump. r99.9's themeRowLabelsFit() did exactly that on Library.
//
// Returns nullptr when `index` has no row in these props: out of the list, before
// the window, past the window, or no array at all. Callers must treat nullptr as
// "no row here" and skip -- a missing row is a visual degradation, never a fault.
const fui::ListItem* propsItemAt(const fui::ListProps& props, const int index) {
  if (props.items == nullptr) return nullptr;
  if (index < 0 || index >= static_cast<int>(props.count)) return nullptr;
  const int first = static_cast<int>(props.itemsWindowFirst);
  if (index < first) return nullptr;
  const int offset = index - first;
  // itemsWindowCount == 0 means "the array runs to the end of the list"
  // (freeink::ui::ListProps' documented contract, and what list.h itself
  // assumes), so only a non-zero count bounds the window.
  if (props.itemsWindowCount > 0 && offset >= static_cast<int>(props.itemsWindowCount)) return nullptr;
  return &props.items[offset];
}

// D1: can THESE props price every row of the list? A measurement of the row set
// needs to see row 0 (the fill it reports IS page 0's), and a windowed list only
// materializes one page. Two things make a props unable to answer:
//   * a window that starts past row 0 -- the scan finds no row for index 0 and
//     gives up without a page-0 fill at all;
//   * a window that stops short of the last row -- the tiling can never reach the
//     end, so the page count it would derive is a guess.
// `rowProvider` lists and bare arrays are not windowed: the provider answers for
// any index, and an unbounded array runs to the end of the list.
bool propsWindowCoversSet(const fui::ListProps& props, const int count) {
  if (props.items == nullptr) return true;  // provider, or nothing to window
  const int first = static_cast<int>(props.itemsWindowFirst);
  // itemsWindowCount == 0 means "the array runs to the end of the list"; it only
  // has to be truthful about where it starts.
  if (props.itemsWindowCount == 0) return first <= 0;
  const int last = first + static_cast<int>(props.itemsWindowCount);
  return first <= 0 && last >= count;
}
}  // namespace

// The label-coverage guard the themed list chrome runs before it binds the row
// face. Two sources, one rule:
//   * props.items -- every list that can materialize its rows up front (settings,
//     chapter lists that are small, network mode, the reader menu);
//   * props.rowProvider -- the provider lists (the file browser formats one row
//     per draw and never holds the array). Probing the page the viewport is on is
//     what matters: the face is rebound on EVERY build, and a rebuild always
//     follows a page move, so a page whose file names are in another script is
//     repainted with the UI face on the very next pass instead of printing holes.
bool UiListActivity::usesThemeListChrome() const {
  // The THEME answers for every list screen that does not speak for itself; the
  // settings tree overrides this to true. One decision point for both.
  return UITheme::getInstance().getTheme().themeListChrome();
}

bool UiListActivity::themeRowLabelsFit(const GfxRenderer& renderer, const int fontId, const fui::ListProps& props) {
  // A face that is not in the font map cannot be measured or drawn, so it cannot
  // be the row face. This is the cheap first answer and it is also the guard that
  // keeps a missing rung a visual degradation instead of a dereference.
  if (!uiFaceLoaded(renderer, fontId)) return false;
  if (props.items != nullptr) {
    // WINDOWED ARRAYS: props.count is the TOTAL row count, props.items is the
    // window. This loop used to run `for (i = 0; i < props.count; ++i) props.items[i]`
    // -- a global index into a page-sized array -- which on Library (count = the
    // whole shelf, items = one page) read past the allocation and panicked the
    // device on entry. Scan the window, through the shared accessor, and skip the
    // indexes it does not cover: an unreadable row is a degradation, not a fault.
    for (int i = 0; i < static_cast<int>(props.count); ++i) {
      const fui::ListItem* item = propsItemAt(props, i);
      if (item == nullptr) continue;
      if (!uiLabelFitsFace(renderer, fontId, item->label)) return false;
    }
    return true;
  }
  if (props.rowProvider == nullptr || props.rowProviderCtx == nullptr) return false;
  // Window: one page of rows from the viewport top (the nav's, not props.topIndex,
  // which the next sync overwrites). kProbeRows covers a full page at the theme
  // pitch with margin, and stays a page-sized cost: a 1000-entry folder formats
  // twelve names per build, not a thousand, and the face is rebound on every
  // build, so a page in another script is corrected on the following pass.
  constexpr int kProbeRows = 12;
  const int count = listCount();
  if (count <= 0) return true;
  const int first = std::clamp(activeNav().top, 0, count - 1);
  const int last = std::min(count, first + kProbeRows);
  fui::ListItem scratch;
  for (int i = first; i < last; ++i) {
    scratch = fui::ListItem{};
    props.rowProvider(props.rowProviderCtx, static_cast<uint16_t>(i), scratch);
    if (!uiLabelFitsFace(renderer, fontId, scratch.label)) return false;
  }
  return true;
}

int UiListActivity::listHeaderBandHeight() const {
  // The band drawChrome() paints IS the band the content margin must clear. A
  // themed list takes the theme's titled height (Andro: 2 x headerHeight), every
  // other list keeps metrics.headerHeight -- the same expression it always used.
  return usesThemeListChrome() ? UITheme::getInstance().getTitledHeaderHeight()
                               : UITheme::getInstance().getMetrics().headerHeight;
}

// r74/r77 theme list chrome. Three theme-owned knobs, each of which means "leave
// the caller's measure alone" when it is 0, so a theme that does not override
// them -- every theme but Andro -- builds the same props it always did, pixel
// for pixel:
//   * the label font (Andro: Amazon Ember 24, the top of the baked ladder)
//   * the row icon box (Andro: 56 px out of the 64 px row masks, i.e. sampled
//     DOWN rather than upscaled from the 32 px list icons)
//   * the row pitch (Andro: the theme's one-page/compact rule -- see
//     BaseTheme::listRowFillPitch and AndroTheme's override: one page at the
//     spread pitch when every row fits at the 92 px minimum, compact 92 px rows
//     only when a pager is genuinely needed)
// The pitch is written into ListProps::rowHeight, the one measure list() uses for
// both the painted row and its tap band, so the two can never drift. A member (not
// a file-local helper) because UiScreen is a class-scoped alias of UiAppHost.
void UiListActivity::applyThemeListChrome(UiScreen& screen, fui::ListProps& props) {
  const BaseTheme& theme = UITheme::getInstance().getTheme();

  // The label face is a FONT SLOT, not a font id: TextStyle::font indexes the
  // target's bound slots (FONT_SMALL/BODY/TITLE/LABEL) and anything past them
  // resolves to FONT_BODY, so writing a font id into labelText.font is a no-op.
  // Bind the slot instead -- r73's row font never reached the glass for exactly
  // that reason.
  //
  // r80: the slot is the THEME'S ROW ROLE, not whatever the caller happened to
  // ask for. A theme that owns the row face binds it into FONT_BODY, so a screen
  // that styles its labels `smallText` draws through FONT_SMALL -- a slot this
  // theme never rebinds -- and keeps the 10 pt UI face. Normalising the role here
  // fixes all of them from one place; themes that supply no row font
  // (fontId == 0) never enter this block, so their props are untouched.
  //
  // r99.9 (operator request D): the face is no longer one fixed size. The theme
  // offers a ladder and fitThemeRowLabels() takes the largest rung at which every
  // label on this screen fits its row on one line, which is what removes the
  // "Clear Read Books from R..." class of truncation from every list at once.
  // The guard is unchanged: the face is only used when it actually carries the
  // labels' codepoints, otherwise the UI body face keeps the row readable.
  const int fontId = theme.listRowFontId();
  if (fontId != 0) {
    props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
    fitThemeRowLabels(screen, props);
  }

  const int iconSize = theme.listRowIconSize();
  if (iconSize > 0) props.iconSize = static_cast<int16_t>(iconSize);

  const int rows = static_cast<int>(props.count);
  const int band = static_cast<int>(screen.body().height);
  const int pitch = theme.listRowFillPitch(band, rows);
  if (pitch <= 0) return;
  props.rowHeight = static_cast<int16_t>(pitch);
  // r79: rowGap stays 0, ALWAYS. The pitch above is the whole row -- paint, tap
  // band and page size are one number. r74-r76 spread the band's leftover as
  // inter-row air (gap = spare / rows), which is what made a short menu's rows
  // read as "excessively tall": a 3-row list on the 1092 px band got 150 px rows
  // with a 214 px gap between them, i.e. 364 px per item, filling the screen. The
  // theme's listRowFillPitch() now caps the pitch itself (one page when the list
  // fits at the minimum measure, compact rows only when a pager is genuinely
  // needed), so there is no leftover to distribute and the block is top-aligned
  // under the header rule as r74 intended.
  props.rowGap = 0;
}

// --- r99.9 (request D): one row face per screen, chosen to fit --------------
// The operator's rule: no menu label may be clipped or ellipsised, and the fix
// must be SHARED so future screens inherit it. So the row face steps down the
// theme's ladder until every label in the row window fits its row on ONE line,
// and the whole screen then uses that one rung.
//
// Why one rung per screen and not per row: on e-ink the compose (~163-165 ms,
// EXP1) dominates a refresh and the row height feeds the dirty mask and the page
// size. A uniform rung keeps every row exactly ListProps::rowHeight tall, so the
// mask, the page count and the pager arithmetic are all what they were; a ragged
// per-row size would grow some rows, change drawnRows, and re-page the screen.
// Wrapping stays available (labelText.maxLines) but only for a label too long
// even for the smallest rung -- and only that row grows.
//
// Cost: the measurement is one layout pass per row per candidate rung, so it is
// signed (width + the page's labels + the preferred face) and cached: a compose
// that shows the same rows at the same width pays one FNV hash over <=14 labels
// (microseconds). Only a page turn, a language switch or a resize re-measures,
// i.e. at most ~14 rows x 4 rungs of layoutText on the build that already
// re-reads the rows -- inside the compose, never an extra panel drive.
void UiListActivity::fitThemeRowLabels(UiScreen& screen, fui::ListProps& props) {
  const BaseTheme& theme = UITheme::getInstance().getTheme();
  // The denser face is a CEILING, not a separate ladder: a file browser starts
  // at Ember 18 and may still step down from there.
  const int denseId = theme.listRowDenseFontId();
  const int preferred = (wantsDenseRowLabels() && denseId != 0) ? denseId : theme.listRowFontId();
  if (preferred == 0) return;

  // The face must carry every label's codepoints. If it does not, keep the UI
  // body face -- rebind on BOTH outcomes, because the slot keeps the last face a
  // build bound and a language switch to another script would otherwise print
  // holes.
  if (!themeRowLabelsFit(renderer, preferred, props)) {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, uiScaleSpec().bodyFontId);
    rowLabelFitFace_ = 0;
    rowLabelFitSig_ = 0;
    return;
  }

  // A screen that materializes its row window AFTER the viewport is measured has
  // nothing to measure here; it keeps the preferred face and calls this again
  // before list() (LibraryListActivity does exactly that).
  if (props.items == nullptr && props.rowProvider == nullptr) {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, preferred);
    return;
  }
  const int count = listCount();
  // The probe range is the rows the SDK is about to DRAW, not the whole list:
  //   * a windowed array (library, TOC, clippings) has exactly
  //     [itemsWindowFirst, itemsWindowFirst + itemsWindowCount) materialized, and
  //     measuring that range is both correct and bounded;
  //   * otherwise a page of rows from the viewport top (the nav's, not
  //     props.topIndex, which the next sync overwrites).
  constexpr int kProbeRows = 14;
  int first = 0;
  int last = 0;
  if (props.items != nullptr && props.itemsWindowCount > 0) {
    first = std::max(0, static_cast<int>(props.itemsWindowFirst));
    last = std::min(count, first + static_cast<int>(props.itemsWindowCount));
  } else if (count > 0) {
    first = std::clamp(activeNav().top, 0, count - 1);
    last = std::min(count, first + kProbeRows);
  }
  const int16_t width = screen.body().width;

  fui::ListItem scratch;
  const auto rowAt = [&](const int index) -> const fui::ListItem* {
    if (props.rowProvider) {
      // The provider contract is a GLOBAL index (freeink::ui::list() calls it with
      // the row's list index), and `scratch` is reused per call, so a returned
      // pointer is only valid until the next rowAt() -- every caller below reads
      // it before asking for the next row.
      scratch = fui::ListItem{};
      props.rowProvider(props.rowProviderCtx, static_cast<uint16_t>(index), scratch);
      return &scratch;
    }
    return propsItemAt(props, index);
  };

  // Signature: the width, the face, and every label this window will show.
  uint32_t sig = 2166136261u;
  for (int i = first; i < last; ++i) {
    const fui::ListItem* item = rowAt(i);
    if (!item) break;
    sig = hashRowText(sig, item->label);
    sig = hashRowText(sig, item->value);
  }
  sig = (sig ^ static_cast<uint32_t>(width)) * 16777619u;
  sig = (sig ^ static_cast<uint32_t>(preferred)) * 16777619u;
  if (sig == rowLabelFitSig_ && rowLabelFitFace_ != 0 && uiFaceLoaded(renderer, rowLabelFitFace_)) {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, rowLabelFitFace_);
    return;
  }

  int ladderCount = 0;
  const int* ladder = theme.listRowFontLadder(ladderCount);
  const fui::DrawTarget& target = screen.frame().target();
  // Candidate rungs: the preferred face, then every SMALLER rung of the ladder.
  // A rung the binary did not bake is SKIPPED rather than bound: an unbaked font
  // id must cost a size, never a measure or a draw on a face that is not there.
  int candidates[8];
  int candidateCount = 0;
  if (uiFaceLoaded(renderer, preferred)) candidates[candidateCount++] = preferred;
  for (int i = 0; ladder != nullptr && i < ladderCount && candidateCount < 8; ++i) {
    if (ladder[i] > 0 && ladder[i] < preferred && uiFaceLoaded(renderer, ladder[i])) {
      candidates[candidateCount++] = ladder[i];
    }
  }
  if (candidateCount == 0) {
    // Nothing on this theme's ladder is present: keep the UI body face rather
    // than bind an id nothing can resolve.
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, uiScaleSpec().bodyFontId);
    rowLabelFitFace_ = 0;
    rowLabelFitSig_ = 0;
    return;
  }

  // Every label on ONE line at the face currently bound into FONT_BODY, using the
  // SAME row geometry list() will draw with (measureListRow resolves the value /
  // toggle / icon columns from these very props).
  const auto fitsOnOneLine = [&]() {
    for (int i = first; i < last; ++i) {
      const fui::ListItem* item = rowAt(i);
      if (!item || item->isHeader || item->label == nullptr || item->label[0] == '\0') continue;
      const fui::ListRowLayout layout = fui::measureListRow(target, nullptr, width, props, *item);
      if (layout.labelWidth <= 0) continue;
      if (target.measureText(props.labelText.font, item->label, props.labelText).width > layout.labelWidth) {
        return false;
      }
    }
    return true;
  };

  int chosen = candidates[candidateCount - 1];
  for (int c = 0; c < candidateCount; ++c) {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, candidates[c]);
    if (fitsOnOneLine()) {
      chosen = candidates[c];
      break;
    }
  }
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, chosen);
  // Last resort for a label too long even for the smallest rung: let it WRAP
  // inside the row's existing slot instead of being ellipsised -- but only when
  // two lines really fit the pitch, so the row does not grow and the screen stays
  // uniform (the operator's "wrap only where the wrapped row still fits the
  // slot"). Screens that asked for a single line (the library) are raised to two;
  // a screen that already allows two keeps what it asked for.
  if (props.labelText.maxLines < 2 && !fitsOnOneLine()) {
    const int16_t lineH = static_cast<int16_t>(target.lineHeight(props.labelText.font));
    if (props.rowHeight > 0 && lineH > 0 && props.rowHeight >= 2 * lineH) props.labelText.maxLines = 2;
  }
  rowLabelFitSig_ = sig;
  rowLabelFitFace_ = chosen;
}

void UiListActivity::drawListWindow(UiScreen& screen, fui::ListProps& props) {
  // The window exists NOW, so this is the first moment the shared fit can see the
  // rows that will be painted. Re-running it here is free when the face already
  // matches (cached against the labels + width), and it is the difference between
  // a ladder that is offered and a ladder that is applied.
  fitThemeRowLabels(screen, props);
  screen.list(props);
}

// --- bottom page-turn buttons ----------------------------------------------
// Only a settings list whose rows do not fit its band gets the pager: the band is
// taken from the bottom of the screen's content through the same Screen the list
// is built into, so the list rect can never overlap them, and each button's hit
// rect is registered by fui::button() from the very rect it fills.
bool UiListActivity::listPagerEnabled() const {
  // RMENU2: the theme's settings-pager flag is still the default answer -- every
  // screen but the reader menu reads exactly what it read before. The reader menu
  // opts in explicitly because it composes through the STOCK theme, which never
  // pages anything, and its rows now need paging anyway.
  return listPagerOptIn() || UITheme::getInstance().getTheme().settingsListPager();
}

bool UiListActivity::reservePageTabs(UiScreen& screen, fui::ListProps& props) {
  pageTabBar_ = fui::Rect{};
  const int count = listCount();
  if (count <= 0 || props.rowHeight <= 0) return false;
  // Theme-gated: only a theme that pages its settings itself (Andro) gets the
  // pager. Every other theme keeps its r73 footer-less, viewport-scroll
  // behaviour, so Classic/Lyra/Lyra3Covers/CoverGrid/RoundedRaff stay
  // byte-identical. RMENU2: a screen that owns its pager (the reader menu, which
  // runs under the stock theme) opts in instead -- see listPagerEnabled().
  if (!listPagerEnabled()) return false;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = std::max(0, metrics.verticalSpacing);
  // The band is the theme's PAGER band, not its tab bar: on Andro this footer is
  // a pair of finger-sized tap targets (68 design -> 92 px, +40 % on the 65 px
  // tabBarHeight it used to borrow), and a theme that does not page its settings
  // list has already returned above. tabBarHeight itself is untouched, so the
  // subheader / reader-menu bands that read it keep their 65 px.
  const int pagerBand = UITheme::getInstance().getTheme().settingsPagerBandHeight();
  const int tabHeight = pagerBand > 0 ? pagerBand : metrics.tabBarHeight;
  if (tabHeight <= 0 || screen.body().height <= tabHeight + gap) return false;
  // r99.9b (operator): the arrows were too SHORT. r99.9's counter line took one
  // line box off the top of the band and left the arrows the rest -- 92 - 31 = 61
  // px on Andro -- which reads as a thin strip next to the counter. Reserve the
  // band for arrows that are 40 % taller than that: the counter's line box is
  // unchanged, the arrows keep everything below it (still edge to edge, still the
  // whole width), and the extra 0.4 x 61 = 24 px comes out of the LIST band, which
  // is what makes them taller without moving the counter or the tap targets. The
  // growth is derived from the measured line height, so a different theme or UI
  // scale keeps the same PROPORTION rather than the same pixels.
  const auto& screenTheme = screen.theme();
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, uiScaleSpec().titleFontId);
  const int readoutH = static_cast<int>(screen.frame().target().lineHeight(screenTheme.titleText.font));
  const int baseArrowH = tabHeight > readoutH ? tabHeight - readoutH : tabHeight;
  int arrowH = baseArrowH + (baseArrowH * 4 + 5) / 10;  // x1.4, rounded
  // RMENU2: a screen may state its OWN arrow floor (the reader menu's 1.5x
  // touch standard). 0 -- the default, and what every settings screen returns --
  // leaves the expression above untouched, so Andro's band is unchanged.
  const int screenArrowMin = listPagerMinArrowHeight(screen);
  if (screenArrowMin > arrowH) arrowH = screenArrowMin;
  const int bandHeight = readoutH + arrowH;
  if (bandHeight <= 0 || screen.body().height <= bandHeight + gap) return false;
  // Does this list page at all? TWO tests, and either may say yes:
  //   * the fixed-height estimate (the pre-U-4 test, kept: it is pure geometry, so it
  //     answers even before the rows can be priced), and
  //   * the row set's own measured TILING (U-4). A list that fits one page at the
  //     uniform pitch but whose rows GROW on one page needs the pager too: without it
  //     that row had no page to be drawn on and no reachable path, because
  //     requestPage() refuses when count <= rows and the swipe degrades to a scroll on a
  //     viewport that cannot move.
  const auto needsPager = [&]() {
    const int estimate = static_cast<int>(fui::listVisibleRows(screen.body(), props.rowHeight, props.rowGap));
    if (estimate > 0 && count > estimate) return true;
    return measureRowSetPageRows(screen, props) > 0 && pagerPageCount() > 1;
  };
  if (!needsPager()) return false;
  const fui::Rect band = screen.takeBottom(static_cast<int16_t>(bandHeight + gap));
  // Re-tile the row set against the body the band just shrank: the page COUNT and the
  // page STARTS the readout and the arrows use come from this measurement
  // (measureRowSetPageRows/pagerPageCount below).
  if (measureRowSetPageRows(screen, props) <= 0) {
    const int rows = static_cast<int>(fui::listVisibleRows(screen.body(), props.rowHeight, props.rowGap));
    if (rows <= 0 || rows >= count) return false;
  } else if (pagerPageCount() <= 1) {
    return false;
  }
  pageTabBar_ = fui::Rect{band.x, static_cast<int16_t>(band.y + gap), band.width, static_cast<int16_t>(bandHeight)};
  return true;
}

// The page TILING of the row set: one pass over every row, priced the way
// freeink::ui::list() prices it (header rows shorter than item rows, a section
// heading attached to a row, rowGap between), filling pageStarts_ with the first row
// index of EVERY page and returning the rows page 0 holds -- the divisor
// pagerPageRows() hands to callers that need a single per-page number.
//
// U-4 (why the starts are per page and not `k * rows`): pricing the set once from
// row 0 and then tiling every page by that count is the "one geometry measured,
// another drawn" defect. list() lays each page out with the rows' REAL heights, so a
// single 2-line label on page 2 made that page draw one row fewer while page 3 still
// started at a multiple of page 0's count -- row 19 was painted on NO page, with the
// readout still printing 3/3 and both arrows live. A list that fitted one page had no
// pager at all, so the dropped tail had no reachable path whatsoever. Flowing the fill
// page by page means page k+1 starts at the first row page k did not draw, so every
// index lands on exactly one page.
//
// A list whose rows are all one height produces exactly the old tiling (starts are
// k * rows), so nothing changes where nothing was wrong. A list with a grown row gets
// a page one row short of full instead of a row that exists nowhere -- the trade until
// MENU2 (adaptive row height) can make the rows uniform again.
//
// 0 = could not measure (no rows, empty band, a row source that ran out mid-scan):
// every reader then falls back to the uniform divisor, i.e. the old behaviour.
int UiListActivity::measureRowSetPageRows(UiScreen& screen, const fui::ListProps& props) {
  const int count = listCount();
  const fui::Rect body = screen.body();
  pageStartCount_ = 0;
  pageStartRowsCount_ = -1;
  pageStartPageRows_ = 0;
  if (count <= 0 || body.height <= 0) return 0;
  const auto& target = screen.frame().target();
  const int gap = props.rowGap > 0 ? props.rowGap : 0;
  const int16_t headerH = props.headerRowHeight > 0
                              ? props.headerRowHeight
                              : static_cast<int16_t>(target.lineHeight(props.headerText.font) + 4);
  int16_t starts[kMaxPagerPages];
  starts[0] = 0;
  int pages = 0;
  int firstPageRows = 0;
  int start = 0;
  fui::ListItem scratch;
  while (start < count) {
    int cursor = 0;
    int filled = start;
    bool ranOut = false;
    for (int i = start; i < count; ++i) {
      const fui::ListItem* item = nullptr;
      if (props.rowProvider) {
        scratch = fui::ListItem{};
        props.rowProvider(props.rowProviderCtx, static_cast<uint16_t>(i), scratch);
        item = &scratch;
      } else {
        item = propsItemAt(props, i);
      }
      if (item == nullptr) {
        // The window ran out: the rows past here are unpriced, so no honest tiling
        // exists for them. Keep the uniform divisor for this list.
        ranOut = true;
        break;
      }
      int h;
      if (item->isHeader) {
        h = headerH + (i != start ? props.sectionGap : 0);
      } else {
        h = fui::measureListRow(target, nullptr, body.width, props, *item).height;
        if (item->sectionHeading != nullptr && item->sectionHeading[0] != '\0') {
          h += headerH + gap + (i != start ? props.sectionGap : 0);
        }
      }
      if (i != start) h += gap;
      if (cursor + h > body.height) break;
      cursor += h;
      filled = i + 1;
    }
    if (ranOut) return firstPageRows;
    const int drawn = filled - start;
    if (drawn <= 0) return firstPageRows;  // not even one row fits this band
    if (pages == 0) firstPageRows = drawn;
    if (pages >= kMaxPagerPages) return firstPageRows;  // too many pages: no map
    starts[pages++] = static_cast<int16_t>(start);
    if (filled >= count) break;
    start = filled;
  }
  if (pages <= 0) return 0;
  for (int k = 0; k < pages; ++k) pageStarts_[k] = starts[k];
  pageStartCount_ = pages;
  pageStartRowsCount_ = count;
  pageStartPageRows_ = firstPageRows;
  return firstPageRows;
}

// THE rows one page moves by: the page size of the ROW SET, measured once, not
// the rows some page happened to draw.
//
// Why not ListNav::pageRowsFor() any more. That returns `drawnRows` -- what the
// LAST layout drew -- and that number belongs to the page being shown, not to the
// list. Page 1 of Settings > Display wraps three labels, so 6 indexes fit; page 2
// wraps none, so 8 fit. pages = ceil(count / rows) therefore printed "1/3" on
// entry and "2/2" one tap later ON THE SAME LIST: the divisor moved with the
// page. e5f8787e could not fix that by reconciling the estimate with the
// measurement, because after the first layout the readout and the arrows were
// already reading the SAME per-page number -- they agreed with each other and
// disagreed with the previous screen state. The bug was never the reconciliation;
// it was that a page count was derived from a per-page measurement at all.
//
// So the page size is now a property of the ROW SET, computed from every row
// before the first paint (syncListViewport -> measureRowSetPageRows), and every
// consumer reads it: the readout, the arrow's target, requestPage() and the
// viewport alignment. It cannot change while you page, so the denominator cannot
// change either.
//
// H2 round 2: WHAT THIS FUNCTION MAY ANSWER FROM, in order, and nothing else --
//   1. the stored measurement for THIS count (pageRowsRowSet_), which only a
//      build that could price every row can install;
//   2. the UNIFORM GEOMETRY DIVISOR, notePagerBand(): (band + gap) / (rowHeight
//      + gap). Two lengths, no row index -- the same number on every page by
//      construction, and the honest answer when the row heights are not reachable
//      (the windowed case);
//   3. the nav's fixed-height estimate, visibleRows -- also listVisibleRows() on
//      the band, i.e. also band-derived -- as a backstop for the frames before the
//      first sync has priced anything.
// It may NEVER answer from ListNav::pageRowsFor()/drawnRows. That was r101.19's
// remaining hole: on the series drill the props reach syncListViewport BEFORE the
// row window is materialised (props.items == nullptr, so propsWindowCoversSet()
// says "not windowed", win=0), the scan therefore finds no rows and returns 0 on
// both pages (meas=-1), and the fallback priced the page on screen -- 9 rows on
// page 1, 8 on page 2 of the same 17 books. The divisor moved with the page, so
// ceil(17/8) printed 2/3 where the list has 2 pages.
int UiListActivity::pagerPageRows() {
  const int count = listCount();
  if (count <= 0) {
    pagerRowsSrc_ = 0;
    return 1;
  }
  // 1. The row set's own tiling, measured from every row of THIS count.
  if (pageRowsRowSetCount_ == count && pageRowsRowSet_ > 0) {
    pagerRowsSrc_ = 1;
    return pageRowsRowSet_;
  }
  // 2. Band + pitch. Never mentions a row, a window or a top, so every page of
  // the same list on the same band answers the same number.
  if (pagerUniformRows_ > 0) {
    pagerRowsSrc_ = 2;
    return pagerUniformRows_;
  }
  // 3. Nothing priced yet (first build of a screen that has not synced): the
  // nav's estimate is listVisibleRows() on the same band, so it is the same class
  // of number -- still not a per-page one.
  const int estimate = activeNav().visibleRows;
  if (estimate > 0) {
    pagerRowsSrc_ = 3;
    return estimate;
  }
  pagerRowsSrc_ = 0;
  return 1;
}

// Price the page from the BAND and the ROW PITCH alone. Called on every build,
// twice: from syncListViewport() with the props the screen built, and again from
// paintPageTabs() with the props list() is really about to lay out -- a windowed
// list (Library) sets its own rowGap AFTER syncListViewport returns, and the
// divisor has to be the pitch that is painted, not the one the theme defaulted.
// Same expression as fui::listVisibleRows(), on purpose: the pager must tile the
// rows the list draws, so it uses the SDK's own arithmetic rather than a copy.
void UiListActivity::notePagerBand(const fui::Rect& band, const fui::ListProps& props) {
  pagerBandPx_ = band.height > 0 ? static_cast<int>(band.height) : 0;
  const int rowH = static_cast<int>(props.rowHeight);
  // A list that never set a pitch has no uniform page: leave the divisor at 0 so
  // pagerPageRows() keeps the row-set measurement (which such a list, having
  // materialised its rows, does have).
  if (pagerBandPx_ <= 0 || rowH <= 0) {
    pagerUniformRows_ = 0;
    return;
  }
  pagerUniformRows_ = static_cast<int>(fui::listVisibleRows(band, props.rowHeight, props.rowGap));
}

const char* UiListActivity::pagerRowsSourceName() const {
  switch (pagerRowsSrc_) {
    case 1:
      return "meas";
    case 2:
      return "uniform";
    case 3:
      return "nav";
    default:
      return "none";
  }
}

// How many pages the row set tiles into: from the measured map when there is one, else
// from the uniform divisor and the count. Same guard as pagerPageRows(): a map measured
// for a different row count belongs to a screen state that no longer exists.
int UiListActivity::pagerPageCount() {
  const int count = listCount();
  if (count <= 0) return 1;
  if (pageStartCount_ > 0 && pageStartRowsCount_ == count) return pageStartCount_;
  const int rows = pagerPageRows();
  return rows > 0 ? (count + rows - 1) / rows : 1;
}

// First row index of `page`. The map's answer when it has one; otherwise the old
// uniform tiling, clamped to the last row.
int UiListActivity::pagerPageStart(const int page) {
  const int count = listCount();
  const int pages = pagerPageCount();
  int p = page < 0 ? 0 : page;
  if (p >= pages) p = pages - 1;
  if (pageStartCount_ > 0 && pageStartRowsCount_ == count) return pageStarts_[p];
  const int start = p * pagerPageRows();
  if (start >= count) return count > 0 ? count - 1 : 0;
  return start;
}

// The page whose window contains `top`. ONE derivation, used by the painted page
// readout and by the swipe / arrow step, so the two inputs can never disagree
// about which page the panel is showing. Paged windows TILE from the top (see
// requestPage()), so the page is simply top / rows -- the tail-page rule the
// viewport-scrolling lists need (their last window starts at count - rows, which
// is not a multiple) has no meaning here and used to file a tiled tail page
// under the page above it.
int UiListActivity::pageIndexForTop(const int top) {
  const int count = listCount();
  if (count <= 0) return 0;
  const int pages = pagerPageCount();
  if (pages <= 1) return 0;
  if (pageStartCount_ > 0 && pageStartRowsCount_ == count) {
    // The map is monotone, so the page is the last start at or before `top`.
    int page = 0;
    for (int k = 1; k < pageStartCount_; ++k) {
      if (pageStarts_[k] <= top) {
        page = k;
      } else {
        break;
      }
    }
    return page;
  }
  const int rows = pagerPageRows();
  if (rows <= 0) return 0;
  int page = (top > 0 ? top : 0) / rows;
  if (page >= pages) page = pages - 1;
  return page;
}

void UiListActivity::paintPageTabs(UiScreen& screen, const fui::ListProps& props) {
  if (pageTabBar_.width <= 0) return;
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Page size and page count come from the SAME measurement the arrow's request
  // and the viewport alignment use -- never from a second estimate taken here.
  const int count = listCount();
  // H2 round 2: re-price the page from THIS frame's band and THIS frame's pitch
  // before any consumer reads it. paintPageTabs() is the one place that holds the
  // props list() is about to lay out, so a windowed screen that sets its own
  // rowGap after syncListViewport (Library) gets its divisor priced on the pitch
  // actually painted -- and, being band + pitch, it prices the same on every page.
  notePagerBand(screen.body(), props);
  const int rows = pagerPageRows();
  // Pages come from the row set's own tiling (U-4), so a page that draws fewer rows than
  // page 0 cannot shift the denominator or make the arrows point past a row nothing drew.
  const int pages = pagerPageCount();
  if (count <= 0 || pages < 2 || rows <= 0) {
    paintedPagerRows_ = -1;
    return;
  }
  // What this frame is about to print, recorded so render() can prove the layout
  // agrees with it before committing the frame (see pagerStaleAfterLayout()).
  paintedPagerRows_ = rows;
  paintedPagerPages_ = pages;
  paintedPagerCount_ = count;
  // The page the panel is showing comes from the page the list is actually built
  // from, not from the selection: the selection is invisible on this touch board
  // and may sit off-page by design.
  const int active = pageIndexForTop(static_cast<int>(props.topIndex));

  // What the readout says: "2/2". Right-aligned, because it now sits at the TOP
  // RIGHT of the pager band, above the arrows rather than between them.
  char readout[12];
  snprintf(readout, sizeof(readout), "%d/%d", active + 1, pages);
  // Evidence for the pager, one line per painted frame: the page size in use, the
  // nav's fixed-height estimate and its measured rows (both per-page numbers, kept
  // only as diagnostics now), the viewport top and the label actually printed. If a
  // readout ever disagrees with itself again, this line says whether the divisor
  // moved, the index moved, or the count did.
  LOG_DBG("PAGER",
          "%s rows=%d pages=%d start=%d est=%d drawn=%d count=%d meas=%d win=%d src=%s top=%u label=%s",
          name.c_str(), rows, pages, pagerPageStart(active), activeNav().visibleRows, activeNav().drawnRows, count,
          pageRowsRowSet_ > 0 ? pageRowsRowSet_ : -1, pagerRowsWindowed_ ? 1 : 0, pagerRowsSourceName(),
          static_cast<unsigned>(props.topIndex), readout);
  fui::TextStyle readoutText = theme.titleText;
  readoutText.align = fui::TextAlign::Right;

  // Button geometry, read from the theme instead of invented: the outline is the
  // theme's rule weight (Physical), the chevron is the theme's chevron box at the
  // theme's chevron stroke, and no length here is scaled by hand -- every number
  // is already converted for this panel.
  //
  // The BOX is the BAR, not the device's 44 px touch minimum: at this panel's 297
  // PPI a 44 px square is 3.8 mm, which is what the operator could not hit (r76).
  // r99.9 (operator request C): the two arrows are ONE bar split down the middle
  // -- '<' takes the left half, '>' the right, they touch, and nothing is
  // withheld between them. The page counter moved out of that seam to the band's
  // top-right corner, on its own line above the arrows, which is what lets the
  // arrows be adjacent and full width at the same time.
  const int16_t rule = static_cast<int16_t>(std::max(1, metrics.headerRuleThickness));
  const int16_t ink = static_cast<int16_t>(std::max(1, metrics.chevronStroke));
  const int16_t boxW = static_cast<int16_t>(std::max(3, metrics.chevronBoxWidth));
  const int16_t boxH = static_cast<int16_t>(std::max(3, metrics.chevronBoxHeight));
  const int16_t touch = std::max<int16_t>(static_cast<int16_t>(44), theme.minTouchSize);
  // The readout is drawn through the FONT_TITLE SLOT, so its face is whatever last
  // bound that slot on this target: a tall Andro titled band binds Ember 24 there,
  // a slider dialog binds NotoSans 18 / Ember 18. Neither is what this band wants
  // (UI_12, below) -- so bind the slot the frame was built with before measuring.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, uiScaleSpec().titleFontId);
  const int readoutPx =
      static_cast<int>(screen.frame().target().measureText(readoutText.font, readout, readoutText).width);
  // Height is the band the theme reserved for the pager, filled edge to edge.
  // r99.9 reserved 68 design -> 92 px and let the counter's line box come off the
  // top, leaving 92 - 31 = 61 px of arrow. r99.9b reserves readoutH + 1.4 x 61 =
  // 116 px instead, so the arrows now stand 85 px tall (+39 % on 61) above this
  // panel's ~54 px touch minimum, with the counter's line box exactly as it was.
  // The 24 px that pays for it comes out of the list band (one row of the 92 px
  // pitch on a full page), never out of the counter or the arrows' width.
  const int16_t readoutH = static_cast<int16_t>(screen.frame().target().lineHeight(readoutText.font));
  const int16_t minArrowH = std::max<int16_t>(touch, static_cast<int16_t>(rule * 2 + boxH));
  const bool stripFits = static_cast<int16_t>(pageTabBar_.height - readoutH) >= minArrowH;
  const int16_t btnY = stripFits ? static_cast<int16_t>(pageTabBar_.y + readoutH) : pageTabBar_.y;
  const int16_t btnH = stripFits ? static_cast<int16_t>(pageTabBar_.height - readoutH) : pageTabBar_.height;
  // Two halves of the bar, tiling it exactly: no seam to withhold, so one arrow
  // can never swallow its neighbour's taps either (the halves never overlap).
  const int16_t btnW = static_cast<int16_t>(pageTabBar_.width / 2);
  const int16_t btnWNext = static_cast<int16_t>(pageTabBar_.width - btnW);
  // No room for a rule-framed box around the chevron: better no button than a
  // glyph wearing a clipped frame.
  if (btnH <= static_cast<int16_t>(rule * 2 + boxH) || btnW <= static_cast<int16_t>(rule * 2 + boxW)) return;

  for (int side = 0; side < 2; ++side) {
    const bool next = side == 1;
    const int target = next ? active + 1 : active - 1;
    // End of the list: there is no page to turn to, so the button is drawn dimmed
    // AND handed enabled = false. fui::button() skips frame.hit() for a disabled
    // button, so a dead direction can neither look live nor act.
    const bool live = target >= 0 && target < pages;
    const int16_t x = next ? static_cast<int16_t>(pageTabBar_.right() - btnWNext) : pageTabBar_.x;
    const fui::Rect rect{x, btnY, static_cast<int16_t>(next ? btnWNext : btnW), btnH};

    fui::ButtonProps btn;
    btn.action = ACTION_PAGE;
    // Clamped, so even the dead side carries an in-range value; `enabled` is what
    // makes it inert, and requestPage() drops an out-of-range page anyway.
    btn.value = static_cast<int16_t>(next ? std::min(active + 1, pages - 1) : std::max(active - 1, 0));
    btn.inputMask = fui::InputTouch;
    btn.enabled = live;
    btn.minTouchSize = touch;
    // Outline only: this theme has no filled selection chrome, and a black
    // rectangle is the shape that ghosts on this panel. A tap repaints nothing but
    // the same rule -- the page turn redraws the list anyway.
    fui::StyleSet styles;
    styles.explicitlySet = true;
    styles.normal.background = fui::Paint::none();
    styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    styles.normal.border = fui::Paint::solid(fui::Color::Black);
    styles.normal.borderWidth = static_cast<uint8_t>(rule);
    styles.selected = styles.normal;
    styles.focused = styles.normal;
    styles.active = styles.normal;
    styles.disabled = styles.normal;
    if (!live) {
      // Half-tone, not a second shape: the frame and the arrow keep their
      // geometry and drop to the dithered ink the theme already uses for
      // "inactive" (CoverGridHomeUi paints its dark card edge the same way).
      styles.disabled.foreground = fui::Paint::dither(fui::Color::LightGray);
      styles.disabled.border = fui::Paint::dither(fui::Color::LightGray);
    }
    btn.styles = styles;
    fui::button(screen.frame(), rect, btn);

    // The chevron, inside the frame the button just painted and on the same
    // centre line, so the ink and the hit rect can never drift apart.
    const fui::Paint arrowInk =
        live ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray);
    const int cx = rect.x + rect.width / 2;
    const int cy = rect.y + rect.height / 2;
    const int halfW = boxW / 2;
    const int halfH = boxH / 2;
    const int tipX = next ? cx + halfW : cx - halfW;
    const int tailX = next ? cx - halfW : cx + halfW;
    chevronArm(screen.frame().target(), tailX, cy - halfH, tipX, cy, ink, arrowInk, live);
    chevronArm(screen.frame().target(), tipX, cy, tailX, cy + halfH, ink, arrowInk, live);
  }

  // Which page is showing, at the TOP RIGHT of the band, on its own line above
  // the arrows. Right-aligned so it reads as a position readout and never
  // competes with the arrows' centre line. UI_12, not the small UI_10: at this
  // panel's 297 PPI the small face sets digits ~15 px (1.3 mm) tall, the body
  // face ~19 px (1.6 mm), and this is the only thing on screen that says where
  // you are. Only painted when the strip could be taken (stripFits) and the
  // digits fit the width -- otherwise the arrows own the whole band and no page
  // count is printed rather than one smeared across their frames.
  if (stripFits && readoutPx + 2 * kReadoutPad <= pageTabBar_.width) {
    screen.frame().target().text(
        fui::Rect{static_cast<int16_t>(pageTabBar_.x + kReadoutPad), pageTabBar_.y,
                  static_cast<int16_t>(pageTabBar_.width - 2 * kReadoutPad), readoutH},
        readout, readoutText);
  }
}

bool UiListActivity::pagerStaleAfterLayout() {
  // Only a paged list has a readout to be wrong, and only a trusted measurement
  // (this layout belongs to this row set) can contradict the one it was painted
  // with. A list whose count changed rebuilt its rows anyway, so it is not stale
  // -- it is simply a different screen state.
  if (pageTabBar_.width <= 0 || paintedPagerRows_ <= 0) return false;
  const int count = listCount();
  if (count != paintedPagerCount_) return false;
  // Compare against the SAME derivation the paint used. The old comparison read
  // ListNav::pageRowsFor() (the rows the last layout drew) while the paint read the
  // same thing one moment earlier -- so after the first layout the two always
  // agreed and this guard could never fire, which is why the "1/3 then 2/2" report
  // survived it. What can still legitimately change between the paint and now is
  // the ROW-SET page size landing late; that is what this catches.
  const int rows = pagerPageRows();
  if (rows <= 0) return false;
  if (rows != paintedPagerRows_) return true;
  // The page COUNT is the tiling's own answer (U-4), and it is what the readout printed.
  // A tiling that lands late is exactly as stale as a late divisor was.
  return paintedPagerPages_ > 0 && pagerPageCount() != paintedPagerPages_;
}

void UiListActivity::pageTabTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  LOG_DBG("LIST", "%s page button -> %d", self->name.c_str(), event.value);
  // Same entry point the swipe uses: an arrow and a gesture cannot diverge.
  self->requestPage(event.value);
}

void UiListActivity::requestPage(const int index) {
  const int count = listCount();
  // A paged list's windows are the tiling's page starts (U-4), not `index * rows`: the
  // page that opens here must start on a row the page before it did not draw.
  const int pages = pagerPageCount();
  if (count <= 0 || pages <= 1) return;
  if (index < 0 || index >= pages) return;
  // Paged windows TILE from the top: page `index` starts at the first row of the tiling's
  // page `index`, and the LAST page simply holds whatever is left. The old rule reused the
  // SDK's tail-page start (count - rows) here, which on a 12-row list paging 9 at a time put
  // "page 2" at row 3 -- so one tap moved three rows, reprinted six, and read as "the arrow
  // did nothing". Offset 0 of the next page is the whole page, always.
  int start = pagerPageStart(index);
  if (start > count - 1) start = count - 1;
  // `start` is a ROW index; nav.selected is a RING index on a tab-ring list, where 0 is
  // the tab bar. Writing the row index straight into the ring landed the selection one
  // row above the page (or, for page 0, ON the tab bar), the build aligned to THAT page,
  // and the viewport snapped back to where it started -- so both arrows and the vertical
  // swipe (which shares this call) looked dead. pendingPageTarget_ stays in ROW space:
  // it is consumed against nav.top, which is a row offset.
  activeNav().requestSelection(start + selectionRingOffset());
  pendingPageTarget_.store(start);
  requestUpdate();
}

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.on(ACTION_PAGE, &UiListActivity::pageTabTrampoline, this);
#if FREEINK_CAP_TOUCH
  revealedIndex.store(-1);
  app.on(ACTION_SWIPE_DELETE, &UiListActivity::swipeDeleteTrampoline, this);
#endif
  app.setScreen(&UiListActivity::screenTrampoline, this);
  // Fresh page state: the first paint of this screen is always committed (HALF,
  // to clean whatever the previous screen left) and always compared against
  // itself, never against the previous screen's page.
  lastRenderedTop_ = -1;
  lastRenderedSignature_ = 0;
  buildSignature_ = 0;
  buildSignatureRows_ = 0;
  buildsSinceRender_ = 0;
  flashCommitFrames_ = 0;
  firstPagedRender_ = true;
  pageTabBar_ = fui::Rect{};
  paintedPagerRows_ = -1;
  paintedPagerPages_ = -1;
  paintedPagerCount_ = -1;
  pageRowsRowSet_ = -1;
  pageRowsRowSetCount_ = -1;
  // H2 round 2: the band/pitch divisor belongs to this screen's geometry. A new
  // screen has not measured its band yet, so nothing may inherit the last one's.
  pagerRowsWindowed_ = false;
  pagerBandPx_ = 0;
  pagerUniformRows_ = 0;
  pagerRowsSrc_ = 0;
  pageStartCount_ = 0;
  pageStartRowsCount_ = -1;
  pageStartPageRows_ = 0;
  rowLabelFitSig_ = 0;
  rowLabelFitFace_ = 0;
  pendingPageTarget_.store(-1);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
#if FREEINK_CAP_TOUCH
  if (self->revealedIndex.load() >= 0) {
    self->closeSwipeDelete();
    self->app.clearTapFlash();
    return;
  }
#endif
  self->onRowAction(event);
}

#if FREEINK_CAP_TOUCH
void UiListActivity::swipeDeleteTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value != self->revealedIndex.load() || !self->canSwipeDelete(event.value)) return;
  self->closeSwipeDelete();
  self->app.clearTapFlash();
  self->swipeDelete(event.value);
}

void UiListActivity::closeSwipeDelete() {
  if (revealedIndex.exchange(-1) >= 0) requestUpdate();
}

void UiListActivity::configureSwipeDelete(UiScreen& screen, fui::ListProps& props) {
  const int row = revealedIndex.load();
  if (row < 0 || row >= listCount() || !canSwipeDelete(row)) return;
  swipeReveal.index = static_cast<int16_t>(row);
  swipeReveal.action = ACTION_SWIPE_DELETE;
  swipeReveal.icon = fui::bitmapFromIcon(icon_trash_2_24);
  swipeReveal.width = static_cast<int16_t>(screen.theme().rowHeight + screen.theme().spaceSm * 2);
  props.reveal = &swipeReveal;
}

bool UiListActivity::handleSwipeDeleteInput() {
  if (!mappedInput.hasTouch()) return false;
  const int openRow = revealedIndex.load();
  if (openRow >= 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      closeSwipeDelete();
      return true;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      fui::Interaction hit;
      bool onDelete;
      {
        RenderLock lock(*this);
        onDelete = routingReady() && app.hitPublished(x, y, ACTION_SWIPE_DELETE, hit) && hit.value == openRow;
        if (!onDelete && routingReady())
          app.route(fui::InputSnapshot{.touchReleased = true, .touchX = -1, .touchY = -1});
      }
      if (!onDelete) {
        closeSwipeDelete();
        return true;
      }
    }
  }

  int x = 0;
  int y = 0;
  const auto swipe = mappedInput.wasSwipe(&x, &y);
  if (swipe == MappedInputManager::SwipeDir::Right && openRow >= 0) {
    UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
    closeSwipeDelete();
    return true;
  }
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    closeSwipeDelete();
    return false;
  }
  if (swipe != MappedInputManager::SwipeDir::Left) return false;

  fui::Interaction hit;
  {
    RenderLock lock(*this);
    if (!routingReady() || !app.hitPublished(x, y, ACTION_ROW, hit) || !canSwipeDelete(hit.value)) return false;
  }
  UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  revealedIndex.store(hit.value);
  requestUpdate();
  return true;
}
#endif

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::List);
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    uisounds::onEvent(uisounds::Event::Back, uisounds::Tag::List);
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) {
      uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::List);
      activateIndex(selected);
    } else {
      // AUDIO1: nothing is selected, so Confirm was refused.
      uisounds::onEvent(uisounds::Event::Error, uisounds::Tag::List);
    }
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::moveSelectionTo(const int index) {
#if FREEINK_CAP_TOUCH
  closeSwipeDelete();
#endif
  // AUDIO3: a move that does not move is SILENT. It used to play the
  // disallowed-action tone here, which made a press at the end of a list (or on
  // a one-page list) sound like an error the reader had committed; it is not one,
  // it is the list saying "there is nothing further that way". The operator's
  // report -- "silent at end of list or single page list" -- is this line.
  // Event::Error stays for refused ACTIONS (Confirm with nothing selected).
  if (uisounds::moveIsAudible(activeNav().selected, index)) {
    uisounds::onEvent(uisounds::Event::MenuMove, uisounds::Tag::List);
  }
  activeNav().requestSelection(index);
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
#if FREEINK_CAP_TOUCH
  if (handleSwipeDeleteInput()) return;
#endif
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    auto& n = activeNav();
    const int page = n.inputPageRows();
    // A paged settings list turns the PAGE, and the selection turns with it: the
    // build's page alignment re-opens whichever page holds the selection, so a
    // viewport-only scroll was snapped straight back to the page the selection
    // still sat on -- the swipe looked dead and only the vol keys turned a page.
    // The selection is invisible on this touch board (r70 paints StateSelected
    // like StateNormal), so moving it costs no ink.
    if (listPagerEnabled() && page > 0 && pageTabBar_.width > 0 && pagerPageCount() > 1) {
      // Which page the viewport is on comes from pageIndexForTop(), the SAME
      // derivation the painted arrows and the page readout use -- this block used
      // to re-derive it as n.top / rows, which filed the tail page (it starts at
      // count - rows, not at a multiple of the row count) under the page above it
      // and left the last page impossible to swipe back out of.
      requestPage(pageIndexForTop(n.top) + (swipe == MappedInputManager::SwipeDir::Up ? 1 : -1));
      return;  // consumed either way: a swipe past the last page is a no-op
    }
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? page : -page;
    n.requestScroll(delta);
    requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  buttonNavigator.onNextPress(
      [this] { moveSelectionTo(ButtonNavigator::nextIndex(activeNav().selected, listCount())); });
  buttonNavigator.onPreviousPress(
      [this] { moveSelectionTo(ButtonNavigator::previousIndex(activeNav().selected, listCount())); });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: with wrapped labels the estimate
  // overshoots and rows between pages would never be shown. The measurement
  // can be one build old while a refresh is in flight; the next layout's
  // feedback corrects the viewport.
  buttonNavigator.onNextContinuous([this] {
    const auto& n = activeNav();
    moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, listCount(), n.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([this] {
    const auto& n = activeNav();
    moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, listCount(), n.inputPageRows()));
  });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const int selectionOffset) {
  // Settings-tree chrome first: the theme may replace the label font, the row
  // icon box and the row measure, and the viewport has to be measured against
  // the SAME props list() paints with or the page size and the tap bands drift.
  if (usesThemeListChrome()) applyThemeListChrome(screen, props);
  // Toggle geometry before the viewport is measured (U-4): measureRowSetPageRows prices
  // each row with measureListRow, which resolves the toggle column from THESE props, so a
  // measurement taken before them would price a row the list does not draw.
  props.toggleCheckbox = true;
  props.toggleWidth = 28;
  props.toggleHeight = 28;
  // A settings list whose rows overflow its band gives up a footer strip for the
  // page tabs BEFORE the viewport is measured, so the page size the nav computes
  // is the page the list will really show (and the list can never overlap the
  // tabs). Painted further down, once the final top is known.
  const bool pageTabsDrawn =
      (usesThemeListChrome() || listPagerOptIn()) && usesListFooterPager() && reservePageTabs(screen, props);
  // Paged, not free-scrolling. The trailing-row preview left a half row clipped
  // at the band's bottom edge, which reads as a truncated row on e-ink and made
  // every swipe land on an arbitrary index. Without it the viewport lays out
  // whole rows only, so the page size the nav measures and the rows the band
  // shows are the same set.
  props.partialTrailingRow = false;
  auto& n = activeNav();
  const int prevTop = n.top;
  const bool trusted = n.trusts(listCount());
  const int drawn = n.drawnRows;

  screen.syncListViewport(n, props, listCount(), selectionOffset);
  // The page size the pager will count with is measured HERE, from every row of
  // the set, before anything is painted or aligned: the readout, the arrow target,
  // requestPage() and the alignment below then all read one number that does not
  // depend on which page happens to be showing. Measuring after the paint is what
  // let the readout print one page count and the next frame another.
  if (pageTabsDrawn) {
    // H2 round 2: price the page from the band the footer left and the pitch these
    // props carry, BEFORE the row-set scan, so a build that cannot see the whole
    // row set (any windowed list) still has a page size -- one that cannot depend
    // on the page, because neither term does.
    //
    // Only when nothing has been priced yet for this screen, or the band moved: at
    // this point the props are the SCREEN's, and a windowed list may still replace
    // props.rowGap before it paints (Library does, after this call). The pitch that
    // is really drawn is only knowable in paintPageTabs(), which re-prices
    // unconditionally -- so its answer is the one worth keeping across builds, and
    // a stale, wider pitch here could snap a page-2 top back to page 1.
    if (pagerUniformRows_ == 0 || pagerBandPx_ != static_cast<int>(screen.body().height)) {
      notePagerBand(screen.body(), props);
    }
    // D1: the window these rows came from decides whether the measurement below
    // can speak for the whole row set. It cannot when the window does not span
    // [0, count) -- the series drill materializes one page at a time -- and in
    // that case a build must NEVER replace a measurement taken on a build that
    // could see row 0. That is the oscillation: page 2's window held 8 of the 17
    // books, the scan priced 8, and ceil(17/8) replaced the true two pages.
    pagerRowsWindowed_ = !propsWindowCoversSet(props, listCount());
    const int measured = measureRowSetPageRows(screen, props);
    if (measured > 0) {
      pageRowsRowSet_ = measured;
      pageRowsRowSetCount_ = listCount();
    } else if (pageRowsRowSetCount_ != listCount()) {
      // A different list: the old measurement describes a row set that is gone.
      pageRowsRowSet_ = -1;
      pageRowsRowSetCount_ = -1;
    }
    // else: KEEP the measurement. It was taken for this exact count from a props
    // that could see every row of it, and a window-limited build has nothing
    // better to offer -- pagerPageRows() refuses to fall back to the rows this
    // page happened to draw and prices the band + pitch instead (src=uniform).
  } else {
    // No pager on this build: the measurement belongs to a screen state that no
    // longer exists, so the next one starts by measuring its own row set.
    pagerRowsWindowed_ = false;
    pagerBandPx_ = 0;
    pagerUniformRows_ = 0;
    pagerRowsSrc_ = 0;
    pageRowsRowSet_ = -1;
    pageRowsRowSetCount_ = -1;
    pageStartCount_ = 0;
    pageStartRowsCount_ = -1;
    pageStartPageRows_ = 0;
  }
  // An explicit page turn (swipe or bottom tab) wins over selection-follow: pin
  // the top this build draws and drop the follow that would otherwise pull the
  // viewport back to where the selection used to be.
  {
    const int wanted = pendingPageTarget_.exchange(-1);
    if (wanted >= 0) {
      // Same page size as the readout and the arrow that asked for it.
      const int rows = std::max(1, pagerPageRows());
      // A PAGED list's last window starts on a page boundary and holds the
      // remainder (see requestPage), so its highest legal top is the last page's
      // start -- NOT count - rows. Clamping to count - rows here is what undid a
      // page turn on any list whose row count is not a multiple of the page: the
      // request asked for row 9 of 12 and this line rewrote it to row 3.
      const int maxTop = pageTabsDrawn ? pagerPageStart(pagerPageCount() - 1) : std::max(0, listCount() - rows);
      n.top = wanted < maxTop ? wanted : maxTop;
      props.topIndex = static_cast<uint16_t>(n.top);
      n.followPending = false;
    }
  }

  // When the selection is already visible in the current viewport (based on
  // the measured drawnRows rather than the unweighted visibleRows estimate),
  // keep selection-follow anchored instead of jumping to top. Explicit swipe
  // scrolling clears followPending and must retain its new viewport.
  if (n.followPending && trusted && drawn > 0) {
    const int sel = props.selectedIndex;
    if (sel >= prevTop && sel < prevTop + drawn) {
      n.top = prevTop;
      props.topIndex = static_cast<uint16_t>(prevTop);
    }
  }

  // Page alignment. With the trailing preview off, a page is a whole number of
  // rows, so its first row must be a whole-row boundary: otherwise "swipe once"
  // and "hold Next" advance by different amounts and a row can be shown twice
  // or skipped depending on where the previous follow correction left `top`.
  // Two steps, in this order, because they can disagree:
  //   1. start the page at or before the nav's answer, snapped down;
  //   2. if that page no longer contains the selection the nav is trying to
  //      show, open the page that DOES contain it. Without step 2 the nav's
  //      one-row-at-a-time follow correction and this snap fight each other and
  //      the render loop's rebuild passes never converge.
  const int count = listCount();
  // pagerPageRows() IS the page size: one number for the arrows, the readout, the
  // request and this alignment.
  const int page = pagerPageRows();
  if (page > 1 && count > 0) {
    const int sel = props.selectedIndex;
    int snapped;
    if (pageTabsDrawn) {
      // PAGED list: the window opens on a PAGE START of the row set's own tiling (U-4),
      // so it is the page the readout prints and the page the arrows asked for -- and a
      // page that drew fewer rows than page 0 cannot leave the rows past its end on no
      // page at all. The selection's page wins when the nav is trying to show a row this
      // page does not contain, which is what keeps the one-row follow correction and this
      // snap from fighting each other across the render loop's rebuild passes.
      const int maxTop = pagerPageStart(pagerPageCount() - 1);
      const int topPage = pageIndexForTop(n.top);
      const int openPage = (sel >= 0 && pageIndexForTop(sel) != topPage) ? pageIndexForTop(sel) : topPage;
      snapped = pagerPageStart(openPage);
      if (snapped > maxTop) snapped = maxTop > 0 ? maxTop : 0;
    } else {
      // Two window rules, by input model:
      //   * a PAGED list (the theme's footer pager) TILES from the top, so its last
      //     window starts at ((count - 1) / page) * page and holds the remainder;
      //   * a viewport-scrolling list keeps the SDK tail page (count - page), which
      //     is the only top from which its final rows fit at all.
      // Same expression either way for the snap-down, they only differ in the
      // ceiling -- and applying the scrolling ceiling to a paged list is what made
      // one arrow tap move three rows instead of nine.
      const int maxTop = count - page > 0 ? count - page : 0;
      snapped = (n.top > 0 ? n.top : 0) / page * page;
      // The tail page starts at count - page, which is not a multiple of the page
      // size. A viewport already sitting there (a swipe or a page tab put it there)
      // is a legitimate page start and must not be snapped back up.
      if (n.top == maxTop && maxTop > 0) snapped = maxTop;
      if (sel >= 0 && (sel < snapped || sel >= snapped + page)) snapped = sel / page * page;
      // The tail page: the window that shows the LAST rows has to start at
      // count - page. Snapping it down instead would make the final rows
      // unreachable by any input.
      if (snapped > maxTop) snapped = maxTop > 0 ? maxTop : 0;
    }
    if (snapped != n.top) {
      n.top = snapped;
      props.topIndex = static_cast<uint16_t>(snapped);
    }
  }

  if (pageTabsDrawn) paintPageTabs(screen, props);

  // Content signature of the rows this build is about to paint. Read by
  // render() to decide whether a buffer flip would change any ink at all; see
  // the skip there for what is (and is not) covered.
  uint32_t sig = 2166136261u;
  int hashed = 0;
  const int first = props.topIndex;
  const int last = std::min(count, first + page + 1);
  fui::ListItem scratch;
  for (int i = first; i < last; ++i) {
    // Same accessor the fit uses: a windowed array is read through its window,
    // never with a global index into props.items.
    const fui::ListItem* item = nullptr;
    if (props.rowProvider) {
      scratch = fui::ListItem{};
      props.rowProvider(props.rowProviderCtx, static_cast<uint16_t>(i), scratch);
      item = &scratch;
    } else {
      item = propsItemAt(props, i);
      if (item == nullptr) break;
    }
    sig = hashRowText(sig, item->label);
    sig = hashRowText(sig, item->value);
    sig = hashRowText(sig, item->subtitle);
    sig = (sig ^ static_cast<uint32_t>(item->state)) * 16777619u;
    sig = (sig ^ (item->toggle ? 0x51u : 0x17u)) * 16777619u;
    sig = (sig ^ (item->enabled ? 0x2Bu : 0x8Du)) * 16777619u;
    ++hashed;
  }
  buildSignature_ = sig;
  // A build that could hash no rows at all -- the windowed TOC/clipping lists
  // materialize props.items AFTER this call, once the viewport is final -- says
  // nothing about its content, and render() must not skip on it.
  buildSignatureRows_ = hashed;
  ++buildsSinceRender_;
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  // A themed list draws the TITLED band; the number comes from the theme and is
  // the same one the screen's content margin subtracts through
  // listHeaderBandHeight() (r74 doubles it on Andro). Every other list screen
  // keeps metrics.headerHeight and therefore its exact r72 band.
  const int bandHeight = listHeaderBandHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), bandHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  // Read BEFORE the paint: FreeInkApp::render() clears the flag, and it is the
  // one signal that says something outside the list changed the frame -- a
  // routed tap, an armed tap flash, a tab switch, a screen reset.
  const bool repaintRequested = app.invalidated();

  // U-1: the compose happens INSIDE the loop below, once per pass. This used to be a full
  // compose of the screen -- clearScreen + drawChrome + renderUi, byte-identical to the loop's
  // first three statements -- that the loop then discarded and immediately repeated, because
  // nothing between the two read any state the first one produced (the pager bookkeeping the
  // loop tests is written inside the compose). One extra 100-200 ms compose on every list frame
  // and on every tap's repaint, for ink that was never flipped.
  // Wrapped labels grow rows, so fewer rows can fit than the fixed-height
  // estimate ListNav plans with. list() reports the real layout back
  // (ListNav::onListRendered); when the selection landed past the drawn rows
  // the nav advanced the viewport and asked for another build. Bounded: top
  // strictly advances toward the selection each pass.
  //
  // The same pass also settles the pager's arithmetic. The first build of a
  // screen has no layout measurement yet, so its page count comes from the
  // fixed-height estimate; if the rows the layout actually drew page differently
  // the readout just painted disagrees with the list under it (the "1/3 then
  // 2/2" bug). pagerStaleAfterLayout() is that comparison, and the extra pass
  // costs one COMPOSE (no extra panel drive -- nothing is flipped between
  // passes), so the frame that reaches the glass always carries a readout the
  // layout it was drawn with agrees with.
  for (int pass = 0;; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
    const bool stalePager = pagerStaleAfterLayout();
    const bool rebuildWanted = activeNav().consumeRebuildNeeded();
    if ((!stalePager && !rebuildWanted) || pass >= 7) break;
  }
  drawFooter();
  // Set by a dispatch that ran during the paint above (a tap that landed inside
  // the render window): it has armed a tap flash, and the frame AFTER it is the
  // one that clears the flash and shows clean ink again. That frame must never
  // be skipped, or the gray flash stays on the panel.
  const bool dispatchArmedFlash = app.invalidated();

  const int top = activeNav().top;
  const bool firstRender = firstPagedRender_;
  const bool pageChanged = top != lastRenderedTop_;
  // WHY THIS SKIP IS SAFE. Every input that changes ink on this screen has to
  // get past one of these five conditions:
  //   firstPagedRender_   -- the entry frame is always committed;
  //   pageChanged         -- a different page means different rows;
  //   buildSignature_     -- the hash of the labels/values/toggles/states this
  //                          build laid out. A data reload that keeps the same
  //                          viewport (a WiFi rescan, a rename, a toggle flip in
  //                          a section that came back) changes it, so it still
  //                          repaints;
  //   repaintRequested    -- anything the app itself invalidated (tap, flash,
  //                          tab switch, reset) as above;
  //   buildsSinceRender_  -- a screen that drew without rebuilding its list is
  //                          not provably unchanged, so it never skips.
  // The SELECTION is deliberately absent from the hash: r70 removed the
  // inverted selection row on this touch board (UIThemeTokens.h paints
  // StateSelected exactly like StateNormal), so moving the selection inside the
  // visible window paints literally nothing. That is the case this skip exists
  // for -- a full-panel repaint of identical pixels is pure flicker on e-ink.
  const bool samePixels = !firstRender && !pageChanged && buildsSinceRender_ > 0 &&
                          buildSignatureRows_ > 0 && buildSignature_ == lastRenderedSignature_ &&
                          !repaintRequested && !dispatchArmedFlash && flashCommitFrames_ == 0;
  lastRenderedTop_ = top;
  lastRenderedSignature_ = buildSignature_;
  buildsSinceRender_ = 0;
  firstPagedRender_ = false;
  // The tap flash is drawn by the frame that dispatches it and cleared at the
  // START of the frame after next, so three frames are involved and the panel
  // shows the gray until the clearing frame is actually FLIPPED. Forbid a skip
  // across all of them.
  if (flashCommitFrames_ > 0) --flashCommitFrames_;
  if (dispatchArmedFlash) flashCommitFrames_ = 3;
  if (samePixels) return;

  // Commit this screen's own waveform instead of inheriting whatever one-shot
  // promotion another call site armed.
  renderer.cancelPromotedRefresh();
  // HALF (GC16 over the diff area) cleans without a from-white blackout, so it
  // is what the entry frame and every page turn get: that is where the old
  // content is replaced wholesale and DU would leave its ghost behind. FAST (DU)
  // is the cheapest drive and never cleans, which is enough when the page is
  // the same page and only some rows changed. Neither is Full: the blanket
  // promotion this replaces cost a ~809 ms blackout per transition.
  //
  // EQ1 (r101.9): Settings > Display > Equalize Black ON converts the HALF half of that pair
  // to EQUALIZE -- the same 48-frame GC16 table with a LUT that re-inks every black pixel
  // page-wide (lut.c:566-592), so a list/settings screen that has accumulated washed-out
  // glyphs comes back solid at no extra drive time. FAST stays FAST: DU has no black row to
  // rewrite, and find_lut_functions() would answer the flag with the GC16 LUT builder, which
  // is not what a DU request means. OFF returns HALF unchanged (src/display/EqualizeBlack.h).
  // r162: ONE return paint asks for HALF. ActivityManager arms this on a pop, where onEnter() is not
  // re-run and `top` is unchanged, so neither `firstRender` nor `pageChanged` can see the arrival and
  // the frame would take FAST -- the grey wipe composite, whose erase prefix pushes the panel to paper
  // (the back-navigation white flash). Consumed here, after the samePixels bail, so a skipped paint
  // cannot eat it and the flag is only ever spent by a paint that actually drives. One shot by
  // construction: the getter clears it.
  const bool returnHalfPaint = consumeHalfPaintOwed();
  if (returnHalfPaint) LOG_INF("GFX", "list return paint: HALF (r162 one-shot return arm)");
  const auto listMode =
      firstRender || pageChanged || returnHalfPaint ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  // SLIDE1: the row this paint is showing was stepped in place, so this drive is
  // ONE diff-masked DU. Consumed HERE -- below the samePixels bail (a skipped
  // paint must not leave the one-shots for another screen's drive) and after
  // cancelPromotedRefresh(), because the list owns its own mode and only the two
  // terms below the policy stage are missing: the row-9 GL16 completion that is
  // the per-step flash, and the shim's grey-wipe lane / clean cadence. Only on a
  // FAST paint: a HALF (entry, page turn) must keep its clean.
  if (valueStepPaint_.exchange(false, std::memory_order_relaxed) && listMode == HalDisplay::FAST_REFRESH) {
    inplacestep::armListPaint(renderer);
    LOG_DBG("GFX", "list value step: one diff-masked DU (no row9 completion)");
  }
  renderer.displayBuffer(equalizeBlackMode(listMode, "list"));
#if CP_ENTRY_SINGLE_DRIVE
  // r97.2: the entry gate can hold this drive while the incoming screen's onEnter() is still building
  // its rows. A held paint never reached the glass, so it must not consume the state this screen keys
  // its entry drive off: put back "this is the first paint" (and drop the signature baseline) so the
  // repaid paint commits HALF over the previous screen instead of a DU diff against it.
  if (renderer.lastPaintAttemptHeld()) {
    firstPagedRender_ = true;
    lastRenderedTop_ = -1;
    lastRenderedSignature_ = 0;
    ++buildsSinceRender_;
    LOG_INF("GFX", "list entry paint was held by the entry gate: first-paint state restored for the repaid paint");
  }
#endif
}
