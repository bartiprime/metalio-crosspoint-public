#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Controller for dictionary lookup: renders the background page, runs the
// lookup against the active StarDict dictionary, and either opens
// DictionaryDefinitionActivity or displays a status popup (Looking up,
// Indexing, Not found, Error).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText = {}, std::string bookTitle = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        lookupText(std::move(lookupText)),
        bookTitle(std::move(bookTitle)) {}

  void onEnter() override;
  void loop() override;
  // Back cancels the lookup and returns to the reader with isCancelled set (see loop()); the manager
  // must not pop this screen itself, because a plain pop would report a NON-cancelled result to the
  // reader and immediately reopen the lookup it was dismissing.
  bool ownsBackKey() const override { return true; }
  void render(RenderLock&&) override;

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

  std::string lookupText;
  // r163 B6: the book this lookup came from, recorded with the word in the vocabulary
  // history. Empty when the lookup has no book behind it (nothing in the reader does).
  std::string bookTitle;
  bool lookupPending = false;
  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
