#include "VocabHistoryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "VocabHistoryStore.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

// Copy `text` into `dst` (a fixed row buffer), always NUL-terminated and never cut
// inside a UTF-8 sequence -- a half codepoint renders as garbage in a row.
void copyRowText(char* dst, const size_t cap, const std::string& text) {
  if (cap == 0) return;
  size_t len = std::min(text.size(), cap - 1);
  while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
  std::memcpy(dst, text.data(), len);
  dst[len] = '\0';
}

}  // namespace

VocabHistoryActivity::VocabHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("VocabHistory", renderer, mappedInput) {}

void VocabHistoryActivity::onEnter() {
  UiListActivity::onEnter();
  windowStart = -1;
  windowCount = 0;
}

int VocabHistoryActivity::listCount() const { return VOCAB_HISTORY_STORE.getCount(); }

const char* VocabHistoryActivity::headerTitle() const { return tr(STR_VOCAB_HISTORY); }

void VocabHistoryActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = std::max(0, start);
  if (clamped > total - ROW_WINDOW) clamped = std::max(0, total - ROW_WINDOW);
  if (clamped == windowStart) return;

  const auto& entries = VOCAB_HISTORY_STORE.getEntries();
  windowCount = std::min(ROW_WINDOW, total - clamped);
  for (int slot = 0; slot < windowCount; ++slot) {
    const auto& entry = entries[static_cast<size_t>(clamped + slot)];
    copyRowText(rowLabel[slot].data(), rowLabel[slot].size(), entry.word);
    copyRowText(rowSubtitle[slot].data(), rowSubtitle[slot].size(), entry.book);

    fui::ListItem item;
    item.label = rowLabel[slot].data();
    item.subtitle = rowSubtitle[slot][0] != '\0' ? rowSubtitle[slot].data() : nullptr;
    item.icon = listIconFor(UIIcon::Text, 32);
    item.actionValue = static_cast<int16_t>(clamped + slot);
    rowItems[slot] = item;
  }
  windowStart = clamped;
}

void VocabHistoryActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

void VocabHistoryActivity::openSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  const std::string word = VOCAB_HISTORY_STORE.getEntries()[static_cast<size_t>(nav.selected)].word;

  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    // The menu row only exists with a dictionary configured, but the setting can change
    // between the menu and here; open lazily rather than trusting it.
    dictOpenOk = SETTINGS.dictionaryName[0] != '\0' && dict.open(SETTINGS.dictionaryName);
  }
  std::string definition;
  std::string headword;
  DictLocation location;
  if (!dictOpenOk || !dict.lookup(word.c_str(), definition, headword, nullptr, &location)) {
    LOG_INF("VOCAB", "history row '%s' is no longer in the dictionary", word.c_str());
    popupMsg = StrId::STR_DICT_NOT_FOUND;
    popupTime = millis();
    showPopup = true;
    requestUpdate();
    return;
  }

  auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                  std::move(definition), dict.definitionsAreHtml());
  if (!activity || !activity->enableLookup(dict, location)) {
    LOG_ERR("VOCAB", "OOM: definition activity");
    popupMsg = StrId::STR_DICT_LOW_MEMORY;
    popupTime = millis();
    showPopup = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

bool VocabHistoryActivity::themeRowLabelsFit(const GfxRenderer& renderer, const int fontId,
                                             const fui::ListProps& props) {
  refreshRowWindow(nav.top);  // no-op when the window already covers this top
  fui::ListProps probe;
  probe.items = rowItems.data();
  probe.count = static_cast<uint16_t>(windowCount);
  return UiListActivity::themeRowLabelsFit(renderer, fontId, probe);
}

void VocabHistoryActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + listHeaderBandHeight()),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  refreshRowWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  // Same shared late-window step as the TOC and the clipping list: apply the row-face
  // ladder to the rows that actually exist, then draw.
  drawListWindow(screen, props);
}

void VocabHistoryActivity::render(RenderLock&& lock) {
  if (showPopup && millis() - popupTime >= POPUP_DURATION_MS) {
    showPopup = false;
    requestUpdate();
  }
  if (showPopup) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  UiListActivity::render(std::move(lock));
}
