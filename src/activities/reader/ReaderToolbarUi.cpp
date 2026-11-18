#include "ReaderToolbarUi.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderMenuChrome.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/readerToolbarIcons.h"
#include "components/icons/readerToolbarIcons64.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_DISMISS = 1;  // tap anywhere on the page above the sheet
constexpr fui::ActionId ACTION_TOOL = 2;     // value = 0 Contents, 1 Text, 2 More
constexpr fui::ActionId ACTION_PREV = 3;     // scrub row: previous chapter
constexpr fui::ActionId ACTION_NEXT = 4;     // scrub row: next chapter
constexpr fui::ActionId ACTION_SCRUB = 5;    // progress track: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;      // panel list row, value = row index

// Scrub row: two small round-cornered chapter buttons flanking a thin progress
// track with a round knob -- the reading page's chrome is light, so the
// controls stay slim rather than control-center sized.
constexpr int16_t kScrubButton = 36;  // chapter step buttons (square, drawn size)
constexpr int16_t kScrubKnob = 16;    // round knob on the 2px progress track
constexpr int16_t kScrubGap = 12;     // air between the buttons and the track
// The project's touch floor. The scrub row is drawn slim (36 px buttons, a 36 px
// band), so the HIT geometry is what has to reach 44 -- the drawn sizes stay put.
// The step buttons grow through hitPadding (ButtonProps::minTouchSize is
// overwritten by the theme's, so padding is the deterministic lever); the
// progress track registers its own rect 44 px tall, centred on the band.
constexpr int16_t kTouchMin = 44;
constexpr int16_t kScrubButtonHitPad = static_cast<int16_t>((kTouchMin - kScrubButton) / 2);  // 4
// Tool row: a 24px glyph centred in each slot, the active slot in an outline
// pill. The whole slot is the tap target; the row height sets its size.
constexpr int16_t kToolRowH = 80;
constexpr int16_t kToolPillInset = 10;
constexpr int kToolCount = 3;
// Bottom sheet height for the panels. ListNav fits whole rows in the remaining
// list area; any spare pixels stay between the list and the switcher.
constexpr int kPanelHeightPercent = 62;
// Cap the sheet may grow to when rounding the list area up to a whole row.
constexpr int kPanelHeightMaxPercent = 72;
// Landscape has less vertical room; leave a narrow page strip for tap-to-dismiss.
constexpr int kLandscapePanelHeightPercent = 88;
}  // namespace

ReaderToolbarUi::ReaderToolbarUi(GfxRenderer& renderer) : UiAppHost(renderer), gfx_(renderer) {}

void ReaderToolbarUi::begin() {
  resetUi();
  pending_ = Routed{};
  nav_.reset();
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_ROW; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() {
  renderUi();
  // Wrapped rows can fit fewer than the fixed-height estimate; the nav then
  // advances the viewport after layout and asks for a rebuild. Converges (top
  // only moves forward toward the selection); the bound is a backstop.
  for (int pass = 0; pass < 3 && nav_.consumeRebuildNeeded(); ++pass) renderUi();
}

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub track is a drag target, so held frames must reach it.
  const auto touch = routeTouch(input, false, /*routeHeld=*/true);
  pending_.routed = touch.routed;
  pending_.x = touch.snap.touchX;
  pending_.y = touch.snap.touchY;
  // Only the release commits a scrub: every held frame dispatches too
  // (dragPermille set), and re-paginating a chapter per frame would be seconds
  // of work per swipe.
  if (pending_.event == Event::Scrub && !touch.snap.touchReleased) pending_.event = Event::None;
  return pending_;
}

void ReaderToolbarUi::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  Routed& out = self->pending_;
  out.value = event.value;
  out.permille = event.dragPermille;
  if (event.action >= ACTION_DISMISS && event.action <= ACTION_ROW) out.event = static_cast<Event>(event.action);
  if (out.event == Event::Scrub && event.dragPermille < 0) out.event = Event::None;
  // A handled action repaints through the reader's own fast path, not through
  // app.invalidate(): the page underneath is the reader's to draw.
  self->app.clearTapFlash();
}

void ReaderToolbarUi::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  if (self->model_.panel) {
    self->buildPanel(screen);
  } else {
    self->buildToolbar(screen);
  }
}

// The Contents / Text / More row: three equal slots, an icon centred in each,
// the active one inside an outline pill (the theme's control radius). Each
// slot is registered as one tap target, so the row stays light (no filled
// tiles, no labels -- the glyphs carry the meaning).
void ReaderToolbarUi::buildToolRow(UiScreen& screen, const fui::LayoutAnchor anchor, const int16_t sideInset) {
  const auto& tokens = screen.theme();
  // r78: the theme owns the glyph box. Andro asks for 40 px and gets the 64 px
  // masks sampled down to it; every other theme returns 0 and keeps the 24 px
  // art this row has always drawn. `glyph` is that resolved size, so the icon
  // rect and the mask choice can never disagree.
  const int themeGlyph = UITheme::getInstance().getTheme().readerToolbarIconSize();
  const int16_t glyph = static_cast<int16_t>(themeGlyph > 0 ? themeGlyph : 24);
  const fui::BitmapRef icons[kToolCount] = {
      themeGlyph > 0 ? fui::bitmapFromIcon(icon_reader_contents_64) : fui::bitmapFromIcon(icon_reader_contents_24),
      themeGlyph > 0 ? fui::bitmapFromIcon(icon_reader_text_64) : fui::bitmapFromIcon(icon_reader_text_24),
      themeGlyph > 0 ? fui::bitmapFromIcon(icon_reader_more_64) : fui::bitmapFromIcon(icon_reader_more_24)};
  // sideInset absorbs the difference between the two hosts' content bands
  // (the toolbar's is spaceLg-inset, the panel's is full width): the slots
  // must land on the same x either way, or the icons jump when a tap swaps
  // the toolbar for a panel.
  const fui::Rect row = screen.take(anchor, kToolRowH).inset(fui::Insets{0, sideInset, 0, sideInset});
  const int16_t slotW = static_cast<int16_t>(row.width / kToolCount);
  // Theme radius as-is (the frontlight panel pattern); the fill clamps to
  // the shape's own height so round themes cannot overshoot.
  const uint8_t pillRadius = tokens.controlRadius;
  for (int i = 0; i < kToolCount; ++i) {
    const fui::Rect slot{static_cast<int16_t>(row.x + slotW * i), row.y, slotW, row.height};
    if (i == model_.activeTool) {
      screen.target().stroke(slot.inset(fui::Insets{4, kToolPillInset, 4, kToolPillInset}),
                             fui::Paint::solid(fui::Color::Black), 2, pillRadius);
    }
    const fui::Rect iconRect{static_cast<int16_t>(slot.x + (slot.width - glyph) / 2),
                             static_cast<int16_t>(slot.y + (slot.height - glyph) / 2), glyph, glyph};
    screen.target().bitmap(iconRect, icons[i], fui::BitmapMode::Center);
    screen.frame().hit(slot, ACTION_TOOL, static_cast<int16_t>(i), fui::InputTouch);
  }
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();

  // r162 item 6: the meta line under the scrub row is the theme's CAPTION face
  // (Andro: Ember 12), the same run of text the list presentation prints its
  // chapter/page line in, so the two presentations of this menu read as one family.
  {
    const GfxRenderer& r = gfx_;
    const int captionFace = readerMenuChrome::captionFace();
    const bool fits = model_.chapterTitle != nullptr && BaseTheme::textFitsFace(r, captionFace, model_.chapterTitle);
    uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, fits ? captionFace : uiScaleSpec().smallFontId);
  }

  // Sheet height from its content: scrub row, meta line, tool row, and the
  // air between them (a full spaceLg under the scrub row, so the progress
  // track and its chapter buttons don't crowd the meta line).
  const int16_t metaH = screen.target().lineHeight(tokens.smallText.font);
  const int16_t contentH = static_cast<int16_t>(tokens.spaceMd + kScrubButton + tokens.spaceLg + metaH +
                                                tokens.spaceSm + kToolRowH + tokens.spaceSm);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  // Grabber air matches the frontlight panel's card language (spaceLg around
  // the grabber, spaceMd more toward the free edge) so the two sheets read as
  // the same family.
  sheetProps.grabberMargin = tokens.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(tokens.spaceLg + tokens.spaceMd);
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  screen.sheet(sheetProps, static_cast<int16_t>(contentH + grabberBand));
  screen.insetContent(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});
  screen.spacer(tokens.spaceMd);

  // Scrub row: < [progress track + knob: tap/drag to jump] >
  {
    const fui::Rect band = screen.takeTop(kScrubButton, tokens.spaceLg);
    stepProps_.label = nullptr;
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_back_24);
    stepProps_.action = ACTION_PREV;
    stepProps_.inputMask = fui::InputTouch;
    stepProps_.styles.explicitlySet = true;
    stepProps_.styles.normal.background = fui::Paint::solid(fui::Color::White);
    stepProps_.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.borderWidth = 1;
    stepProps_.styles.normal.radius = tokens.controlRadius;
    stepProps_.styles.selected = stepProps_.styles.normal;
    stepProps_.styles.focused = stepProps_.styles.normal;
    stepProps_.styles.disabled = stepProps_.styles.normal;
    stepProps_.styles.active = stepProps_.styles.normal;
    stepProps_.styles.active.background = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.active.foreground = fui::Paint::solid(fui::Color::White);
    // 36 px drawn; the hit rect reaches kTouchMin through symmetric padding, so
    // the target grows into the band's air rather than into the track (48 px of
    // separation keeps both rects clear even on a theme with a 54 px floor).
    stepProps_.hitPadding = fui::Insets{kScrubButtonHitPad, kScrubButtonHitPad, kScrubButtonHitPad,
                                        kScrubButtonHitPad};
    screen.button(stepProps_, fui::Rect{band.x, band.y, kScrubButton, kScrubButton});
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_next_24);
    stepProps_.action = ACTION_NEXT;
    screen.button(stepProps_,
                  fui::Rect{static_cast<int16_t>(band.right() - kScrubButton), band.y, kScrubButton, kScrubButton});

    // Progress: a 2px track with a solid round knob at the book position (the
    // fill edge IS the handle, so nothing else is drawn), the scrub hit rect
    // kTouchMin tall and centred on the band so a finger a little off the line
    // still scrubs. Tap-to-jump / drag arrive as dragPermille along this rect.
    const int16_t trackX = static_cast<int16_t>(band.x + kScrubButton + kScrubGap);
    const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kScrubButton + kScrubGap));
    const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 1), trackW, 2}, ink);
    const int permille = std::clamp(model_.progressPermille, 0, 1000);
    const int16_t knobX = static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - 1) * permille) / 1000);
    screen.target().fill(fui::Rect{static_cast<int16_t>(knobX - kScrubKnob / 2),
                                   static_cast<int16_t>(trackY - kScrubKnob / 2), kScrubKnob, kScrubKnob},
                         ink, static_cast<uint8_t>(kScrubKnob / 2));
    screen.frame().hit(fui::Rect{trackX, static_cast<int16_t>(band.y - (kTouchMin - band.height) / 2), trackW,
                                 kTouchMin},
                       ACTION_SCRUB, 0, fui::InputTouch | fui::InputDrag);
  }

  // Meta line: chapter title (left), chapter page / book percent (right).
  {
    const fui::Rect line = screen.takeTop(metaH, tokens.spaceSm);
    fui::TextStyle titleStyle = tokens.smallText;
    titleStyle.bold = true;
    fui::TextStyle infoStyle = tokens.smallText;
    infoStyle.align = fui::TextAlign::Right;
    const int16_t infoW =
        model_.pageInfo ? screen.target().measureText(infoStyle.font, model_.pageInfo, infoStyle).width : 0;
    const fui::Rect titleRect{line.x, line.y, static_cast<int16_t>(line.width - infoW - tokens.spaceMd), line.height};
    if (model_.chapterTitle) screen.target().text(titleRect, model_.chapterTitle, titleStyle);
    if (model_.pageInfo) screen.target().text(line, model_.pageInfo, infoStyle);
  }

  buildToolRow(screen, fui::LayoutAnchor::Top, 0);  // content band already spaceLg-inset
}

void ReaderToolbarUi::buildPanel(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();

  // r162 item 6: this sheet is the TOOLBAR presentation of the reader's in-book
  // menu, so it wears the same chrome as the LIST presentation -- the menu's row
  // pitch and row face from ReaderMenuChrome.h, the theme's titled face for the
  // panel title and its caption face for the page counter. Before this the sheet
  // kept the SDK's default row (68 px) in the UI body face, so the same menu
  // looked like two unrelated surfaces depending on Reader Menu Style.
  //
  // The face is bound into the SLOT (TextStyle::font is a slot, not a font id) and
  // only when it carries the labels, exactly as the list guards its rows: Ember has
  // no registered fallback, so a panel of non-Latin labels keeps the UI face rather
  // than printing holes. Four probe rows bound the cost; every label of a reader
  // menu comes from the same handful of StrIds anyway.
  const int menuFace = readerMenuChrome::rowFace(gfx_);
  bool faceCovers = menuFace != 0 && model_.rowText != nullptr;
  for (int probe = 0; faceCovers && probe < 4 && model_.itemCount > 0; ++probe) {
    const int index = (model_.itemCount - 1) * probe / 3;
    const std::string label = model_.rowText(index);
    faceCovers = BaseTheme::textFitsFace(gfx_, menuFace, label.c_str());
  }
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, faceCovers ? menuFace : uiScaleSpec().bodyFontId);

  // Title in the theme's titled face (Andro: Ember 24), guarded on the string.
  {
    const int titleFace = readerMenuChrome::titleFace();
    const bool titleFits =
        model_.panelTitle != nullptr && BaseTheme::textFitsFace(gfx_, titleFace, model_.panelTitle);
    uiTarget.setFont(fui::GfxRendererTarget::FONT_TITLE, titleFits ? titleFace : uiScaleSpec().titleFontId);
    // Caption face for the page counter ("2/3": digits only, so it always fits).
    uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, readerMenuChrome::captionFace());
  }

  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;  // tap the page above the sheet = back to the toolbar
  // Same grabber air as the toolbar sheet / frontlight panel.
  sheetProps.grabberMargin = tokens.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(tokens.spaceLg + tokens.spaceMd);

  // Size the sheet to an exact number of list rows instead of a fixed screen
  // share: a percentage leaves up to a row's height of dead space above the
  // switcher, and a short list (Text, More) leaves empty row slots. Chrome =
  // everything in the sheet that is not the list; the row count starts from
  // the target share, grows one row when that still fits the cap, and shrinks
  // to the item count when the list is shorter than the space.
  const int16_t titleH = screen.target().lineHeight(tokens.titleText.font);
  // The rows take the MENU pitch, not the SDK's default row: this sheet shows the
  // same controls as the list presentation, so it shows them at the same 66 px
  // (1.5x the touch minimum) rather than the 68 px default or the theme's 92 px
  // browse pitch. A button board keeps the theme's denser row, as every list does
  // there.
  const int16_t rowH = model_.denseRows ? static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight)
                                        : readerMenuChrome::rowPitch(gfx_);
  // rowGap is the theme's gap on both paths: with the menu pitch the row IS the
  // whole stride (the list presentation sets rowGap 0 for the same reason), and a
  // button board keeps its denser row with the theme's gap under it.
  const int16_t rowGap = tokens.listRowGap;
  const int16_t rowStride = static_cast<int16_t>(rowH + rowGap);
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  const int16_t chrome =
      static_cast<int16_t>(grabberBand + titleH + tokens.spaceMd + tokens.spaceSm + kToolRowH + tokens.spaceSm);
  const bool landscape = safe.width > safe.height;
  const int16_t target =
      static_cast<int16_t>((safe.height * (landscape ? kLandscapePanelHeightPercent : kPanelHeightPercent)) / 100);
  const int16_t cap =
      static_cast<int16_t>((safe.height * (landscape ? kLandscapePanelHeightPercent : kPanelHeightMaxPercent)) / 100);
  int sheetRows = (target - chrome + rowGap) / rowStride;
  if (static_cast<int16_t>(chrome + (sheetRows + 1) * rowStride - rowGap) <= cap) ++sheetRows;
  if (model_.itemCount > 0 && sheetRows > model_.itemCount) sheetRows = model_.itemCount;
  if (sheetRows < 1) sheetRows = 1;
  screen.sheet(sheetProps, static_cast<int16_t>(chrome + sheetRows * rowStride - rowGap));
  // No blanket side inset: Screen::list() draws in the content band, and the
  // scroll track must reach the sheet's edge like a full-screen list's does.
  // The title insets itself; the rows inset via rowInset below.

  // Title line: panel name left, page position right when the list spans pages.
  {
    fui::TextStyle titleStyle = tokens.titleText;
    titleStyle.bold = true;
    const fui::Rect line =
        screen.takeTop(titleH, tokens.spaceMd).inset(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});
    screen.target().text(line, model_.panelTitle, titleStyle);
    // Filled in below once the viewport is known; reserve the rect now.
    pageIndicatorRect_ = line;
  }

  // Switcher row along the sheet's bottom edge; the list takes what is left.
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);
  buildToolRow(screen, fui::LayoutAnchor::Bottom, tokens.spaceLg);  // full-width band
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);

  listProps_.count = static_cast<uint16_t>(std::max(0, model_.itemCount));
  listProps_.action = ACTION_ROW;
  listProps_.inputMask = fui::InputTouch;  // physical buttons stay with the reader
  listProps_.rowHeight = rowH;
  listProps_.rowGap = rowGap;
  listProps_.toggleCheckbox = true;
  listProps_.toggleWidth = 28;
  listProps_.toggleHeight = 28;
  // The label column starts flush with the panel title (no list-side padding
  // on top of the sheet's own inset). Body-size text: small reads condensed
  // and the taller row doubles as the tap target.
  listProps_.labelText = tokens.bodyText;
  listProps_.sidePadding = 0;
  // The list band spans the sheet's full width -- the scroll track hugs the
  // panel edge (theme bezel inset included) exactly like a full-screen list.
  // rowInset pulls the rows back to the title's spaceLg alignment.
  listProps_.rowInset = tokens.spaceLg;
  const fui::Rect listRect = screen.body();
  const int count = std::max(0, model_.itemCount);
  // The nav owns selection + viewport (same fui::ListNav idiom as the list
  // menu screens). A shown cursor re-follows into view on every build; a
  // hidden one (-1, touch) leaves the viewport where scrolling put it.
  nav_.selected = std::clamp(model_.selectedIndex, -1, count - 1);
  nav_.followOnBuild = nav_.selected >= 0;
  nav_.followPending = false;
  nav_.syncToProps(listRect, listProps_.rowHeight, rowGap, count, listProps_);

  // Materialise only the visible window of rows.
  const int windowCount = std::min({nav_.visibleRows, count - nav_.top, kMaxWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = nav_.top + i;
    windowLabels_[i] = model_.rowText ? model_.rowText(index) : std::string();
    windowValues_[i] = model_.rowValue ? model_.rowValue(index) : std::string();
    fui::ListItem item;
    item.label = windowLabels_[i].c_str();
    item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
    if (model_.rowCheckbox) model_.rowCheckbox(model_.rowCheckboxContext, index, item);
    item.actionValue = static_cast<int16_t>(index);
    windowItems_[i] = item;
  }
  listProps_.items = windowItems_;
  listProps_.itemsWindowFirst = static_cast<uint16_t>(nav_.top);
  listProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  listProps_.valueText = tokens.bodyText;
  listProps_.valueText.bold = true;
  if (count > 0) {
    screen.list(listProps_);
  }

  const int pageRows = nav_.pageRows();
  const int totalPages = pageRows > 0 ? (count + pageRows - 1) / pageRows : 0;
  if (totalPages > 1) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", nav_.top / pageRows + 1, totalPages);
    fui::TextStyle pageStyle = tokens.smallText;
    pageStyle.align = fui::TextAlign::Right;
    screen.target().text(pageIndicatorRect_, buf, pageStyle);
  }
}
