#pragma once

#include <array>

#include "VocabHistoryStore.h"
#include "activities/UiListActivity.h"
#include "util/Dictionary.h"

// r163 B6: the words this card has looked up, most recent first, each with the book it
// came from. A plain list screen in the same shape as the clipping list: the base class
// owns the loop, the viewport and the row dispatch, this class owns the data and what a
// row does.
//
// Rows are materialised in a fixed window (ROW_WINDOW entries) because the store's
// strings are copied into fui::ListItem labels, and a full 40-row copy on every build
// would cost more heap than the list is worth.
//
// Activating a row looks the word up again in the active dictionary and opens the same
// definition screen the reader uses, so the history is a way back into the dictionary
// rather than a dead record. With no dictionary configured the row is inert and the
// header says why.
class VocabHistoryActivity final : public UiListActivity {
 public:
  VocabHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  // The row window is materialised after syncListViewport, so the base's scan of
  // props.items would find nothing; scan our own window instead.
  bool themeRowLabelsFit(const GfxRenderer& renderer, int fontId, const freeink::ui::ListProps& props) override;

  void openSelected();
  void refreshRowWindow(int start);

  static constexpr int ROW_WINDOW = 8;
  std::array<freeink::ui::ListItem, ROW_WINDOW> rowItems;
  // Label/subtitle storage for the rows: fui::ListItem keeps a pointer, not a copy.
  std::array<std::array<char, VocabHistoryStore::WORD_MAX_CHARS>, ROW_WINDOW> rowLabel;
  std::array<std::array<char, VocabHistoryStore::BOOK_MAX_CHARS>, ROW_WINDOW> rowSubtitle;
  int windowStart = -1;
  int windowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;

  // One-line feedback for a row that cannot be opened (no dictionary, or the word is
  // gone from it). Same transient shape as the lookup screen's popup.
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
  bool showPopup = false;
};
