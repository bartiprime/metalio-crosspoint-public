#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

// EOB1: the one place the theme/device tokens become the pure table's inputs.
// Every number is read from UITheme (scaled metrics) or the renderer (font line
// heights), never assumed: the host test feeds the same values the Plus derives
// (bodyLine 51, touch floor 65, verticalSpacing 14, sidePadding 27).
eobpanel::Tokens panelTokens(const GfxRenderer& renderer, const size_t rows) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  eobpanel::Tokens t;
  t.safe = eobpanel::Box{safe.x, safe.y, safe.width, safe.height};
  t.bodyLine = renderer.getLineHeight(UI_12_FONT_ID);
  t.smallLine = renderer.getLineHeight(UI_10_FONT_ID);
  // uiThemeTokens(): minTouchSize = max(44, body line height + 14).
  t.minTouchSize = t.bodyLine + 14;
  if (t.minTouchSize < 44) t.minTouchSize = 44;
  t.verticalSpacing = metrics.verticalSpacing;
  t.sidePadding = metrics.listSidePadding;
  t.rowCount = static_cast<int>(rows);
  return t;
}

// Context for a window repaint: the row table plus the window's first index.
// Lives on the caller's stack inside paintRowWindow — the provider runs inside
// screen.list(), i.e. before that slot goes out of scope.
struct EobRowWindow {
  EndOfBookOptions* self = nullptr;
  int first = 0;
};

// The panel's rows are 1-bit chrome, so the selection is shown by a frame, not
// by a filled bar: the shared touch theme paints StateSelected exactly like a
// normal row (UIThemeTokens.h: "StateSelected is not a navigation affordance"),
// which on this modal list meant the buttons moved a selection nobody could see.
fui::StyleSet eobRowStyles() {
  fui::StyleSet s;
  s.explicitlySet = true;
  s.normal.background = fui::Paint::solid(fui::Color::White);
  s.normal.foreground = fui::Paint::solid(fui::Color::Black);
  s.selected = s.normal;
  s.selected.border = fui::Paint::solid(fui::Color::Black);
  s.selected.borderWidth = 3;
  s.focused = s.selected;
  s.active = s.selected;
  s.disabled = s.normal;
  return s;
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector.store(0, std::memory_order_relaxed);
  if (!names.empty()) {
    // One-time app setup on the render task, before the first render/route.
    resetUi();
    app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
    app.setScreen(&EndOfBookOptions::listScreen, this);
    buildRowItems();
  }
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list (rowItems/rowLabels included)
  isLoaded.store(true, std::memory_order_release);
}

// Populates rowLabels/rowItems from names + the trailing "Home" row. Called
// once here since names never changes after loadOnce() completes.
void EndOfBookOptions::buildRowItems() {
  rowCount = 0;
  for (const auto& name : names) {
    if (rowCount >= MAX_ROWS) break;
    rowLabels[rowCount] = displayName(name);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  if (rowCount < MAX_ROWS) {
    rowLabels[rowCount] = tr(STR_EOB_HOME);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire) && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || event.value > static_cast<int16_t>(self->names.size())) return;
  self->selector.store(event.value, std::memory_order_relaxed);
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
  self->tappedRow = event.value;
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent record the tapped row.
  tappedRow = -1;
  const auto route = routeTouch(input);
  // cppcheck can't see that route() dispatches into onRowEvent (registered via
  // app.on(ACTION_ROW, ...)), which sets tappedRow, so it flags this as always false.
  // cppcheck-suppress knownConditionTrueFalse
  if (route && tappedRow >= 0) {
    if (tappedRow < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(tappedRow);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" row tapped
  }
  if (route.routed && app.invalidated()) {
    return Action::Redraw;
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selectedIndex);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiScreen& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

eobpanel::Layout EndOfBookOptions::panelLayout(const GfxRenderer& renderer) const {
  return eobpanel::layout(panelTokens(renderer, rowCount));
}

fui::ListProps EndOfBookOptions::rowProps(const GfxRenderer& renderer, const eobpanel::Layout& layout,
                                          const int first, const int count, void* windowCtx) const {
  (void)renderer;
  fui::ListProps props;
  const bool whole = (first == 0 && count == static_cast<int>(rowCount));
  if (whole) {
    props.items = rowItems;
    props.count = static_cast<uint16_t>(rowCount);
    props.selectedIndex = static_cast<int16_t>(selector.load(std::memory_order_relaxed));
  } else {
    // Window: count is the window size, and the row index is pulled by the
    // provider (a whole-array list clamps its own topIndex to count - visible,
    // so it would draw the wrong two rows out of a four-row array).
    props.count = static_cast<uint16_t>(count);
    props.rowProvider = &EndOfBookOptions::windowRowProvider;
    props.rowProviderCtx = windowCtx;
    props.selectedIndex = static_cast<int16_t>(selector.load(std::memory_order_relaxed) - first);
  }
  props.topIndex = 0;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  // EOB1 geometry: the row height/gap the table computed, so the rects are the
  // same on every paint and a two-row repaint cannot drift from the entry frame.
  props.rowHeight = static_cast<int16_t>(layout.rowHeight);
  props.rowGap = static_cast<int16_t>(layout.rowGap);
  props.sidePadding = static_cast<int16_t>(layout.sidePadding);
  props.labelText.bold = true;
  props.rowStyles = eobRowStyles();
  return props;
}

// The window's row source: index 0 is row `first`.
void EndOfBookOptions::windowRowProvider(void* ctx, uint16_t index, fui::ListItem& item) {
  const auto* window = static_cast<const EobRowWindow*>(ctx);
  const int row = window->first + static_cast<int>(index);
  if (window->self == nullptr || row < 0 || row >= static_cast<int>(window->self->rowCount)) return;
  item = window->self->rowItems[row];
}

void EndOfBookOptions::buildListScreen(UiScreen& screen) {
  const eobpanel::Layout layout = panelLayout(renderer);
  if (layout.rowCount == 0) {
    return;
  }
  const int lastBottom = layout.rows[rowCount - 1].y + layout.rows[rowCount - 1].h;
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(layout.rows[0].y),
      static_cast<int16_t>(renderer.getScreenWidth() - (layout.band.x + layout.band.w)),
      static_cast<int16_t>(renderer.getScreenHeight() - lastBottom),
      static_cast<int16_t>(layout.band.x)});

  // rowLabels/rowItems were built once in loadOnce() (see buildRowItems())
  // and reused here on every repaint.
  screen.list(rowProps(renderer, layout, 0, static_cast<int>(rowCount)), 0);
}

// EOB1: a selection repaint, without a clear. A local Frame is enough: the
// rows need their rects registered nowhere (the published interaction table
// from the entry frame already carries the same rects, which is the property
// that lets this paint skip renderUi()), so the throwaway buffer is discarded.
void EndOfBookOptions::paintRowWindow(GfxRenderer& renderer, const eobpanel::Layout& layout,
                                      const int first, const int count) {
  if (first < 0 || count <= 0) return;
  const int lastBottom = layout.rows[first + count - 1].y + layout.rows[first + count - 1].h;
  EobRowWindow window{this, first};
  fui::InteractionBuffer<6> scratch;
  fui::Frame<6> frame(uiTarget, uiTarget.deviceContext(), fui::InputSnapshot{}, scratch, nullptr);
  fui::Screen<6> screen(frame, app.theme());
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(layout.rows[first].y),
      static_cast<int16_t>(renderer.getScreenWidth() - (layout.band.x + layout.band.w)),
      static_cast<int16_t>(renderer.getScreenHeight() - lastBottom),
      static_cast<int16_t>(layout.band.x)});
  screen.list(rowProps(renderer, layout, first, count, &window), 0);
}

void EndOfBookOptions::drawChrome(GfxRenderer& renderer, const MappedInputManager& input,
                                  const eobpanel::Layout& layout) {
  const Rect safe{layout.safe.x, layout.safe.y, layout.safe.w, layout.safe.h};
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, layout.header.y, tr(STR_END_OF_BOOK), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, layout.subtitle.y, tr(STR_EOB_CONTINUE_WITH));

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

bool EndOfBookOptions::selectionChanged(int* first, int* last) const {
  const int now = selector.load(std::memory_order_relaxed);
  const int shown = renderedSelector;
  if (now == shown || rowCount == 0) return false;
  int lo = now < shown ? now : shown;
  int hi = now < shown ? shown : now;
  if (lo < 0) lo = 0;
  if (hi >= static_cast<int>(rowCount)) hi = static_cast<int>(rowCount) - 1;
  if (hi < lo) return false;
  if (first) *first = lo;
  if (last) *last = hi;
  return true;
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  if (!menuActive()) {
    // No suggestions: the historical plain end screen. 3/8 of the screen height matches
    // the previous fixed position on the 480x800 panel and scales to other resolutions.
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    paintedLayout = panelLayout(renderer);
    chromePainted = true;
    renderedSelector = selector.load(std::memory_order_relaxed);
    return;
  }

  // Suggestion menu: title, list (+ Home entry) and button hints. The hints are drawn at
  // the physical front buttons, which is a logical side/top edge in the rotated
  // orientations — lay out inside the safe area so nothing hides behind them.
  const eobpanel::Layout layout = panelLayout(renderer);
  drawChrome(renderer, input, layout);

  // The list renders through the FreeInkApp so its rows register touch hit
  // rects; renderUi re-derives the device context, picking up any rotation
  // since construction (reader menu rotate).
  renderUi();
  paintedLayout = layout;
  chromePainted = true;
  renderedSelector = selector.load(std::memory_order_relaxed);
}

bool EndOfBookOptions::chromeValid() const {
  if (!chromePainted) return false;
  const eobpanel::Layout now = panelLayout(renderer);
  // The rows must be where the entry frame painted them: same band, same row
  // height, same count. Anything else is a full paint, not a two-row tick.
  return now.rowCount == paintedLayout.rowCount && now.rowHeight == paintedLayout.rowHeight &&
         now.band.x == paintedLayout.band.x && now.band.y == paintedLayout.band.y &&
         now.band.w == paintedLayout.band.w;
}

bool EndOfBookOptions::renderSelection(GfxRenderer& renderer) {
  int first = -1;
  int last = -1;
  if (!selectionChanged(&first, &last)) return false;
  if (!chromeValid()) return false;  // geometry moved: the caller must repaint in full
  const eobpanel::Layout layout = panelLayout(renderer);
  paintRowWindow(renderer, layout, first, last - first + 1);
  renderedSelector = selector.load(std::memory_order_relaxed);
  return true;
}
