#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/bars/tab-bar.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class CoverGridHomeUi final : public UiAppHost {
 public:
  static constexpr int THUMB_HEIGHT = 400;
  static constexpr int GRID_COLUMNS = 3;
  static constexpr int GRID_ROWS = 2;
  static constexpr int MAX_BOOKS = 1 + GRID_COLUMNS * GRID_ROWS;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);
  // Interaction action ids. SELECT's values are book slots (hero/0, grid index+1); the tab row has
  // its own id so a tab value can never alias a book index. Public because Home has to name which
  // action resolved its tap.
  static constexpr freeink::ui::ActionId ACTION_SELECT = 1;
  static constexpr freeink::ui::ActionId ACTION_TAB = 2;
  explicit CoverGridHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasOpds, bool hasContinueReading);
  void refreshCoverPaths();
  void setSelection(int selection) { selected = selection; }
  // Tab hits come back separately: a tab tap must never be expressible as a book index.
  int selectedAction(const MappedInputManager& input, int* tabOut = nullptr);
  // WHICH action answered the last resolved tap (0 = nothing resolved), plus its value. Home needs
  // the id, not just the value: a tab slot and a book slot live in different action spaces, and a
  // value alone cannot say which band was hit.
  freeink::ui::ActionId lastActionId() const { return lastActionId_; }
  int lastActionValue() const { return lastActionValue_; }
  // Exact generation height, shared by every slot and recorded during draw.
  // Thumbs must be generated at the drawn size: rescaling a dithered 1-bit
  // image aliases badly.
  int thumbHeightFor() const;
  bool takeThumbHeightChanged();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  static void onTabAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHeaderBand();
  void drawEmpty(UiScreen& screen);
  void drawCurrent(UiScreen& screen, freeink::ui::Rect rect, int coverRowHeight);
  void drawGrid(UiScreen& screen);
  freeink::ui::Rect layoutGrid(freeink::ui::Rect rect);
  void drawTabs(UiScreen& screen, freeink::ui::Rect rect);
  bool paintFramedCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);
  void refreshCoverPath(size_t index);
  void noteThumbHeight(int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int selected = 0;
  int pending = -1;
  // Tab taps land here, never in `pending`: the two live in different action spaces, so no tab
  // value can ever be mistaken for a book index.
  int pendingTab = -1;
  // Result of the last selectedAction() call: which action fired (0 = none) and with what value.
  freeink::ui::ActionId lastActionId_ = 0;
  int lastActionValue_ = -1;
  int progress = -1;
  bool hasOpds = false;
  bool hasContinueReading = false;
  char progressText[12]{};
  // Component styles and interaction tables stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::Rect gridBounds{};
  freeink::ui::TabBarProps tabs;
  // r112: six slots -- File Browser, Library, Apps, [Plugins/OPDS], File transfer,
  // Settings. The tab row and the classic home list are the SAME order, so Home can
  // map a tab value straight to a home-menu index.
  std::array<freeink::ui::TabItem, 6> tabItems;
  // Compressed tab slot -> the full slot it came from (the Plugins/OPDS tab is absent
  // when nothing is configured, so the painted icon cannot be derived from the value).
  std::array<uint8_t, 6> tabSlot{0, 1, 2, 3, 4, 5};
};
