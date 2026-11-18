#include "EpubReaderMenuActivity.h"

#include <BlePageTurner.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>

#include "CrossPointSettings.h"
#include "CadencePicker.h"
#include "MappedInputManager.h"
#include "ReaderMenuChrome.h"
#include "ReaderUtils.h"
#include "VocabHistoryStore.h"
#include "audio/UiSounds.h"
#include "components/TitledHeaderBand.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

// r162 item 6 -- THE READER'S IN-BOOK MENU RESTYLED, NOT ROUTED.
//
// History, because this screen has now been wrong in both directions:
//
//   * RMENU1/RMENU2 pointed the whole compose at the DEFAULT theme through a
//     scoped theme swap ("renders as upstream does"), so the menu and the rest
//     of the UI became two different families. That is not a restyle, it is
//     routing, and it is gone: the swap helper is deleted, nothing can install a
//     theme for one screen any more, and this screen renders through the ACTIVE
//     theme like every other one.
//   * Leaving the theme's LIST chrome on is equally wrong. themeListChrome()
//     hands a list this theme's browse-list numbers: the 108 px doubled app bar,
//     the 6 px band rule, the 92 px settings pitch and the 56 px icon box
//     sampled from the 64 px ROW masks. The operator saw exactly that -- a
//     doubled app bar (the 108 px bar plus the progress strip sitting right under
//     its rule, reading as a second bar) and browse-list rows around a menu that
//     has no icons and only 14-19 short controls.
//
// So the menu takes the theme's VOCABULARY and none of its list geometry: the
// faces, the caption face, the spacing unit and the side rail come from the
// theme, while the band height, the row pitch and the row icon box are derived
// for a menu of this length (ReaderMenuChrome.h -- the SAME numbers the toolbar
// presentation of this menu reads, so the two cannot drift apart).
//
// buildMenuItems() is untouched: Night Mode and Frontlight remain NOT rows here
// (r99.9 A4 -- both live in the swipe-down panel), and no row was added.


EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool hasClippings)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(menuItems, hasFootnotes, hasBookmarks, hasClippings);
  buildMenuRowItems();
}

// Populates menuRowItems's labels/actionValue from menuItems. Called once
// here since menuItems (and thus which rows exist) never changes after
// construction; buildScreen() only touches the two rows with a live value.
void EpubReaderMenuActivity::buildMenuRowItems() {
  for (size_t i = 0; i < menuItems.size() && i < MAX_MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i].labelId);
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, const bool hasFootnotes,
                                            const bool hasBookmarks, const bool hasClippings) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (hasClippings) {
    items.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  // r99.9 (operator request A4): Night Mode and Frontlight are NOT rows here.
  // Both are already in the swipe-down control panel (FrontlightPanelActivity:
  // the Night Mode tile and the Brightness/Warmth sliders), so a row here is a
  // duplicate entry for a control the operator reaches in one gesture. The
  // MenuAction values and their handlers stay put -- the setting itself
  // (screenInverted), the frontlight state and the panel are untouched, and
  // the panel remains the one place they are reachable from.
  if (bleturner::status().compiledIn) {
    items.push_back({MenuAction::BLUETOOTH, StrId::STR_BT_PAGE_TURNER});
  }
  if (SETTINGS.dictionaryName[0] != '\0') {
    items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  }
  // r163 B6: the vocabulary history sits beside Look Up, and only when there is
  // something in it -- an empty list is not worth a menu row.
  if (SETTINGS.dictionaryName[0] != '\0' && VOCAB_HISTORY_STORE.getCount() > 0) {
    items.push_back({MenuAction::VOCAB_HISTORY, StrId::STR_VOCAB_HISTORY});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  // Reader cadences live here, beside the other reading controls, not in the frontlight sheet: they
  // are a reading decision, and the sheet is a lighting decision.
  items.push_back({MenuAction::QUICK_CADENCE, StrId::STR_QUICK_REFRESH_PAGES});
  items.push_back({MenuAction::FULL_CADENCE, StrId::STR_REFRESH_FREQ});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
}

void EpubReaderMenuActivity::toggleBluetooth() {
  SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
  if (!SETTINGS.ble.enabled) bleturner::switchOff();
  SETTINGS.saveToFile();
  // Enabling waits for the main-loop lifecycle's reader heap gates, then says in the status bar
  // whether a remote linked.
  if (SETTINGS.ble.enabled) bleturner::requestConnect();
}

void EpubReaderMenuActivity::closeCancelled() {
  // AUDIO3: leaving the More menu is one of the surfaces that used to be
  // completely silent -- its handleButtons() is overridden, so UiListActivity's
  // own Back emission never ran for this screen.
  uisounds::onEvent(uisounds::Event::Back, uisounds::Tag::ReaderMenu);
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

// CAD2: the picker's list, in DISPLAY order -- 1/5/10/15/30/60/Never -- not the
// enum's order. The stored byte is still the REFRESH_FREQUENCY slot (see
// CrossPointSettings.h: append-only, persisted by index); CadenceOrder.h owns the
// position <-> slot mapping, and the Settings > Reader rows read the SAME table
// (SettingsList.h buildCadenceSetting), so the two pickers cannot show different
// orders. CadencePicker.h is where the table is checked against the enum.
using cadenceorder::kCount;
using cadenceorder::kLabels;
static_assert(kCount == CrossPointSettings::REFRESH_FREQUENCY_COUNT,
              "the cadence display table must have one entry per REFRESH_FREQUENCY slot");

static uint8_t& cadenceSetting(const bool quick) {
  return quick ? SETTINGS.quickRefreshPages : SETTINGS.refreshFrequency;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const auto selectedAction = menuItems[index].action;
  LOG_DBG("MENU", "activate index=%d of %u action=%d", index, static_cast<unsigned>(menuItems.size()),
          static_cast<int>(selectedAction));
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::QUICK_CADENCE || selectedAction == MenuAction::FULL_CADENCE) {
    const bool quick = selectedAction == MenuAction::QUICK_CADENCE;
    // The marked row is the stored slot's DISPLAY position, not the slot itself: a
    // device saved on Never marks the bottom entry, and picking the bottom entry
    // writes Never (5) back. Out-of-range positions write nothing.
    optionPopup.show(menuItems[index].labelId, kLabels.data(), kCount,
                     cadenceorder::positionOf(cadenceSetting(quick)), [this, quick](int pos) {
                       uint8_t stored = 0;
                       if (!cadenceorder::storedForDisplay(pos, stored)) return;
                       if (cadenceSetting(quick) == stored) return;
                       cadenceSetting(quick) = stored;
                       SETTINGS.saveToFile();
                       LOG_INF("MENU", "%s cadence -> slot %u (display %d)", quick ? "quick" : "full",
                               static_cast<unsigned>(stored), pos);
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::NIGHT_MODE) {
    SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::BLUETOOTH) {
    toggleBluetooth();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // AUDIO3: same override as Back -- the base class's Select never runs here.
    // A row TAP already sounds through UiListActivity::onRowAction, so this is
    // only the physical button, and the two cannot double up.
    uisounds::onEvent(uisounds::Event::Select, uisounds::Tag::ReaderMenu);
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  // The band this screen paints, and the margin under it, are ONE number:
  // menuBandHeight() is what drawChrome() gives the shared band helper, and it is
  // what the content margin clears here. Two expressions is how a themed list's
  // band and its content origin drifted apart in the first place.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int16_t bandPx = menuBandHeight();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + bandPx),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // Progress summary: a CAPTION line, not a second band.
  //
  // It used to take metrics.tabBarHeight (65 px) off the content -- a strip as
  // tall as a footer directly under the band, which is what made the operator read
  // two app bars. It is now sized to its own line box in the theme's caption face
  // (Andro: Ember 12, the face its status bar uses) and separated from the rows by
  // the theme's own vertical rhythm, so the band is the only chrome on the screen.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const fui::ThemeTokens& tokens = screen.theme();
  const int captionFace = readerMenuChrome::captionFace();
  // Ember has no registered fallback, so a caption in a script it does not carry
  // keeps the UI face rather than printing holes -- the same guard the themed
  // lists apply to their rows.
  if (!BaseTheme::textFitsFace(renderer, captionFace, progressLine.c_str())) {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, uiScaleSpec().smallFontId);
  } else {
    uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, captionFace);
  }
  const int16_t captionH = screen.target().lineHeight(tokens.smallText.font);
  const fui::Rect captionLine = screen.takeTop(captionH);
  const int16_t sidePad = static_cast<int16_t>(metrics.listSidePadding);
  screen.target().text(captionLine.inset(fui::Insets{0, sidePad, 0, sidePad}), progressLine.c_str(),
                       tokens.smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // menuRowItems's labels/actionValue were set once in the constructor (see
  // buildMenuRowItems()); only rows with live values need refreshing here.
  for (size_t i = 0; i < menuItems.size(); i++) {
    const auto action = menuItems[i].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      menuRowItems[i].value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      menuRowItems[i].value = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::QUICK_CADENCE) {
      // The row's value is the label of the STORED slot (Never stays "Never"
      // wherever it now sits in the list).
      menuRowItems[i].value = I18N.get(kLabels[cadenceorder::positionOf(SETTINGS.quickRefreshPages)]);
    } else if (action == MenuAction::FULL_CADENCE) {
      menuRowItems[i].value = I18N.get(kLabels[cadenceorder::positionOf(SETTINGS.refreshFrequency)]);
    } else if (action == MenuAction::NIGHT_MODE) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.screenInverted);
    } else if (action == MenuAction::BLUETOOTH) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.ble.enabled);
    } else if (action == MenuAction::FRONTLIGHT) {
      GUI.setCheckboxRow(menuRowItems[i], Frontlight.isOn());
    }
  }

  fui::ListProps props;
  // The page counter and the title do not share a band, by construction rather
  // than by nudging pixels: the title is painted by drawChrome() into the single
  // band at the top of the screen, and the counter is painted by the shared pager
  // into the band reservePageTabs() takes off the BOTTOM of this content area
  // (screen.takeBottom). takeBottom shrinks screen.body(), so the pager band and
  // the list cannot overlap either. So: title in the header, counter right-aligned
  // in the pager band's top strip, and no shared line to collide on.
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(menuItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge

  // THE ROW MEASURE. Three numbers, each derived for a menu rather than borrowed
  // from the browse list (see ReaderMenuChrome.h):
  //
  //   pitch   = touchStandard() = ceil(1.5 x the 44 px touch minimum) = 66 px. The
  //             project rule is 50 % above the minimum, and it is also the same
  //             number the pager's arrows use, so the two cannot drift.
  //   face    = the largest rung of the theme's own ladder whose line box still
  //             leaves 8 px above and below inside that pitch: Ember 20 (a 50 px
  //             box). Ember 24, the theme's browse-list face, is a 60 px box and
  //             would leave 3 px -- a block, not a row.
  //   icon    = none. No row in this menu has an icon, so the theme's 56 px box
  //             (which samples the 64 px ROW masks) would be dead air.
  //
  // At 66 px the 14-19 row menu tiles in 1-2 pages of the ~1090 px content band,
  // against 92 px browse rows: the menu stays a menu instead of becoming a list.
  const int16_t pitch = readerMenuChrome::rowPitch(renderer);
  int face = readerMenuChrome::rowFace(renderer);
  if (face != 0) {
    // Ember has no registered fallback: a label in a script it does not carry
    // keeps the UI body face instead of printing holes.
    for (size_t i = 0; i < menuItems.size() && face != 0; ++i) {
      if (!BaseTheme::textFitsFace(renderer, face, menuRowItems[i].label)) face = 0;
    }
  }
  // Bind the SLOT: TextStyle::font is a slot index (FONT_SMALL/BODY/TITLE), so
  // writing a font id into labelText.font is a no-op -- the trap that kept r73's
  // row font off the glass.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, face != 0 ? face : uiScaleSpec().bodyFontId);
  props.labelText = tokens.bodyText;
  props.labelText.maxLines = 2;  // a label too long even for the rung wraps, and only it grows
  props.valueText = tokens.bodyText;
  props.valueText.bold = true;  // the value is the data, the label is the control
  props.iconSize = readerMenuChrome::rowIconBox();
  props.rowHeight = pitch;
  // rowGap 0, ALWAYS: the pitch IS the row -- paint, tap band and page size are
  // one number (the same rule the theme's listRowFillPitch() states). Left at its
  // -1 sentinel the SDK would inherit the active theme's gap and then raise it to
  // listTouchRowGap (6), which would make the stride 72 while every other
  // derivation here says 66.
  props.rowGap = 0;
  syncListViewport(screen, props);
  screen.list(props);
}

// RMENU2: the '<' / '>' controls meet the same 1.5x floor as the rows, from the
// SAME derivation (ReaderMenuChrome::rowPitch). Where that is taller than the
// strip the theme reserved the arrows set the band, which is the layout
// consequence of the standard -- the pager costs about one menu row of band,
// never a shorter tap target.
int EpubReaderMenuActivity::listPagerMinArrowHeight(const UiScreen& screen) const {
  (void)screen;
  return readerMenuChrome::rowPitch(renderer);
}

void EpubReaderMenuActivity::drawChrome() {
  // ONE band paint, by the SHARED titled-band helper -- the same recipe Settings,
  // the library and the catalog family use, so this screen cannot grow a second
  // one -- with the two departures this screen needs handed to it as options
  // rather than re-implemented:
  //
  //   bandHeightPx    menuBandHeight(): 80 px, sized by the theme's own title face
  //                   and app-bar button, NOT the 108 px browse-list app bar.
  //   paintThemeRule  FALSE. RDR1 (operator): the reader's horizontal divider is
  //                   gone in the Andro theme, so drawTitledBandRule() -- Andro's
  //                   6 px band stroke -- is not called for this screen at all.
  //
  // That is the whole of the "doubled app bar" and the "6 px band stroke": there
  // is one band, and its bottom edge carries no ink. The helper's other two
  // edge-painting mechanisms are already dead on this theme (paintsOwnBandRule()
  // suppresses the frame border; headerUnderlineSize is 0), so the count of
  // strokes under this band is ZERO, not "one".
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int16_t bandPx = menuBandHeight();
  fui::GfxRendererFrame<1> ui(renderer, uiScaleSpec().smallFontId, uiScaleSpec().bodyFontId,
                              uiScaleSpec().titleFontId);
  fui::HeaderProps props;
  titledHeaderBand::BandOptions options;
  options.paintThemeRule = false;
  options.bandHeightPx = bandPx;
  // The band's edge inset is the theme's LIST rail, so the leading arrow and the
  // row labels below share one left edge. (The title itself starts past the 72 px
  // leading slot, as on every app bar -- that is the header's own structure, not
  // something this screen can put on the row rail.)
  options.sidePadding = static_cast<int16_t>(metrics.listSidePadding);
  titledHeaderBand::drawTitledHeaderBand(GUI, ui.frame, renderer,
                                        Rect{safe.x, static_cast<int16_t>(safe.y + metrics.topPadding), safe.width,
                                             bandPx},
                                        title.c_str(), nullptr, props, /*backButton=*/true, options);
}

int16_t EpubReaderMenuActivity::menuBandHeight() const { return readerMenuChrome::bandHeight(renderer); }

void EpubReaderMenuActivity::render(RenderLock&& lock) {
  // A live popup is a DIALOG, and dialogs are the active theme's: paint it and
  // leave. Nothing about this screen's chrome applies to it.
  if (optionPopup.processRender(renderer, mappedInput)) return;

  // The active theme, like every other screen. No theme is swapped, installed or
  // scoped for this screen -- that mechanism is deleted (see the file comment).
  UiListActivity::render(std::move(lock));
}

