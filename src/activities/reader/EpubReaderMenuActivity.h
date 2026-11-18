#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    BLUETOOTH,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    QUICK_CADENCE,
    FULL_CADENCE,
    ROTATE_SCREEN,
    BOOKMARKS,
    SAVE_CLIPPING,
    VIEW_CLIPPINGS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY,
    // r163 B6: the words looked up from this card, newest first.
    VOCAB_HISTORY
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static void toggleBluetooth();
  static void buildMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks, bool hasClippings);

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes, bool hasBookmarks,
                                  bool hasClippings);

  // Render path: this screen paints with the ACTIVE theme like every other one.
  // Its own chrome numbers (band height, row pitch, row face, icon box) come from
  // ReaderMenuChrome.h, shared with the toolbar presentation of the same menu.
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
  bool acceptsRemoteInput() const override { return routingReady(); }

 private:
  // Row storage: menuItems is at most MAX_MENU_ITEMS, so a
  // fixed-capacity array avoids any heap allocation for the row list. Labels
  // are set once in the constructor (buildMenuRowItems()); buildScreen() only
  // refreshes rows whose values reflect live state.
  static constexpr size_t MAX_MENU_ITEMS = 21;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};
  void buildMenuRowItems();

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  // r162 item 6: the theme's LIST chrome is DECLINED here. themeListChrome() would
  // hand this menu the browse-list numbers -- the 108 px doubled app bar, the 6 px
  // band rule, the 92 px settings pitch and the 56 px icon box -- and a menu of
  // 14-19 iconless controls is not a browse list. The theme's tokens and faces are
  // still used; only its list geometry is not. buildScreen() writes this screen's
  // own pitch/face/icon box and drawChrome() paints its own band height.
  bool usesThemeListChrome() const override { return false; }
  // The pager: the rows are 66 px (1.5x the touch minimum) so a full menu can need
  // a second page. Both hooks route to UiListActivity's existing machinery
  // (reservePageTabs/paintPageTabs/pageIndexForTop/requestPage) -- no second pager.
  bool usesListFooterPager() const override { return true; }
  bool listPagerOptIn() const override { return true; }
  // Same 1.5x floor as the rows (ReaderMenuChrome::rowPitch), so the '<' and '>'
  // controls are never the one touch target below the standard.
  int listPagerMinArrowHeight(const UiScreen& screen) const override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back closes on RELEASE and Confirm activates on RELEASE; everything else
  // (row navigation, page jumps) falls through to the base handler.
  bool handleButtons() override;
  // The ONE header path: a single band at menuBandHeight(), painted by the shared
  // titled-band helper with the theme's rule suppressed (RDR1 -- the reader has no
  // divider under its band in the Andro theme).
  void drawChrome() override;
  // This screen's band height, in panel pixels. drawChrome() and buildScreen()'s
  // content margin both read THIS, so the band and the content under it cannot
  // drift apart.
  int16_t menuBandHeight() const;

  void closeCancelled();

  // Fixed menu layout
  std::vector<MenuItem> menuItems;

  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
