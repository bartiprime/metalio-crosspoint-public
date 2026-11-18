#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "activities/reader/ReaderHandoff.h"

using readerhandoff::CompletionSlots;
using readerhandoff::DisplayLockGuard;
using readerhandoff::SessionGeneration;
using readerhandoff::SessionIdentity;

namespace {

SessionIdentity identity(int spine, uint32_t layout, const void* section) {
  SessionIdentity id;
  id.section = section;
  id.spineIndex = spine;
  id.layoutSignature = layout;
  return id;
}

struct Done {
  int handle = 0;
  bool ok = false;
};

}  // namespace

// ---- session generation -------------------------------------------------------------------

TEST(SessionGeneration, FirstObservationIsTheFirstSession) {
  SessionGeneration gen;
  const uint32_t before = gen.current();
  EXPECT_TRUE(gen.observe(identity(3, 0xAA, nullptr)));
  EXPECT_EQ(gen.current(), before + 1);
  EXPECT_TRUE(gen.live(gen.current()));
}

// A repaint of the same page in the same chapter is not a new session: idle work prepared
// across a status-bar repaint must survive it.
TEST(SessionGeneration, SameIdentityDoesNotBump) {
  SessionGeneration gen;
  const int section = 0;
  gen.observe(identity(3, 0xAA, &section));
  const uint32_t after = gen.current();
  EXPECT_FALSE(gen.observe(identity(3, 0xAA, &section)));
  EXPECT_FALSE(gen.observe(identity(3, 0xAA, &section)));
  EXPECT_EQ(gen.current(), after);
}

TEST(SessionGeneration, SpineSectionOrLayoutChangeBumps) {
  SessionGeneration gen;
  const int a = 0;
  const int b = 1;
  gen.observe(identity(3, 0xAA, &a));
  const uint32_t base = gen.current();

  EXPECT_TRUE(gen.observe(identity(4, 0xAA, &a)));  // next chapter
  EXPECT_EQ(gen.current(), base + 1);
  EXPECT_TRUE(gen.observe(identity(3, 0xAA, &a)));  // back to the first one: still a new session
  EXPECT_EQ(gen.current(), base + 2);
  EXPECT_TRUE(gen.observe(identity(3, 0xBB, &a)));  // layout changed under us
  EXPECT_EQ(gen.current(), base + 3);
  EXPECT_TRUE(gen.observe(identity(3, 0xBB, &b)));  // a different Section object
  EXPECT_EQ(gen.current(), base + 4);
}

TEST(SessionGeneration, OldGenerationIsNotLive) {
  SessionGeneration gen;
  gen.observe(identity(1, 1, nullptr));
  const uint32_t old = gen.current();
  gen.observe(identity(2, 1, nullptr));
  EXPECT_FALSE(gen.live(old));
  EXPECT_TRUE(gen.live(gen.current()));
}

// ---- completion slots ---------------------------------------------------------------------

TEST(CompletionSlots, PublishThenPollReturnsTheSameResult) {
  CompletionSlots<Done, 2> slots;
  EXPECT_EQ(slots.occupied(), 0);
  EXPECT_TRUE(slots.publish(7, Done{42, true}));
  EXPECT_EQ(slots.occupied(), 1);

  Done out;
  EXPECT_TRUE(slots.poll(7, out));
  EXPECT_EQ(out.handle, 42);
  EXPECT_TRUE(out.ok);
  EXPECT_EQ(slots.occupied(), 0);
  EXPECT_FALSE(slots.poll(7, out));
}

TEST(CompletionSlots, PollIsOldestFirst) {
  CompletionSlots<Done, 4> slots;
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(slots.publish(1, Done{i, true}));
  Done out;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(slots.poll(1, out));
    EXPECT_EQ(out.handle, i);
  }
  EXPECT_FALSE(slots.poll(1, out));
}

// A full slot set refuses: the producer must be able to tell that its completion was NOT
// advertised, instead of it being silently swallowed or overwriting one already waiting.
TEST(CompletionSlots, PublishRefusesRatherThanOverwriting) {
  CompletionSlots<Done, 3> slots;
  EXPECT_TRUE(slots.publish(1, Done{0, true}));
  EXPECT_TRUE(slots.publish(1, Done{1, true}));
  EXPECT_TRUE(slots.publish(1, Done{2, true}));
  EXPECT_FALSE(slots.publish(1, Done{3, true}));
  EXPECT_EQ(slots.occupied(), 3);

  // The refused publish left the three waiting results untouched.
  Done out;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(slots.poll(1, out));
    EXPECT_EQ(out.handle, i);
  }
}

// A result from a session that is gone is dropped, not applied -- and dropping it does not
// block the live result queued behind it.
TEST(CompletionSlots, StaleGenerationIsDroppedNotReturned) {
  CompletionSlots<Done, 3> slots;
  EXPECT_TRUE(slots.publish(4, Done{1, false}));  // from the chapter we just left
  EXPECT_TRUE(slots.publish(5, Done{2, true}));   // current session

  Done out;
  ASSERT_TRUE(slots.poll(5, out));
  EXPECT_EQ(out.handle, 2);
  EXPECT_FALSE(slots.poll(5, out));  // the stale one was consumed here, not returned
  EXPECT_EQ(slots.occupied(), 0);
}

TEST(CompletionSlots, OnlyStaleResultsDrainToEmpty) {
  CompletionSlots<Done, 2> slots;
  EXPECT_TRUE(slots.publish(1, Done{1, true}));
  EXPECT_TRUE(slots.publish(1, Done{2, true}));
  Done out;
  EXPECT_FALSE(slots.poll(99, out));
  EXPECT_EQ(slots.occupied(), 0);
}

// Refusing then accepting again after the consumer advances must work: the ring reuses the slot.
TEST(CompletionSlots, RefusalClearsOnceTheConsumerPolls) {
  CompletionSlots<Done, 2> slots;
  EXPECT_TRUE(slots.publish(3, Done{1, true}));
  EXPECT_TRUE(slots.publish(3, Done{2, true}));
  EXPECT_FALSE(slots.publish(3, Done{3, true}));
  Done out;
  ASSERT_TRUE(slots.poll(3, out));
  EXPECT_TRUE(slots.publish(3, Done{3, true}));
  ASSERT_TRUE(slots.poll(3, out));
  EXPECT_EQ(out.handle, 2);
  ASSERT_TRUE(slots.poll(3, out));
  EXPECT_EQ(out.handle, 3);
}

// ---- display guard ------------------------------------------------------------------------

// Rule 4 as a type: Try is the only acquisition the reader path can make, so there is no way to
// ask the guard to wait on a compose, and the guard is not a value that can be handed around.
TEST(DisplayLockGuard, CannotBeAskedToBlock) {
  EXPECT_TRUE(std::is_default_constructible_v<DisplayLockGuard>);
  EXPECT_FALSE((std::is_constructible_v<DisplayLockGuard, RenderLock::Mode>));
  EXPECT_FALSE((std::is_constructible_v<DisplayLockGuard, RenderLock::Mode, bool>));
  EXPECT_FALSE((std::is_convertible_v<RenderLock, DisplayLockGuard>));
  EXPECT_FALSE((std::is_copy_constructible_v<DisplayLockGuard>));
  EXPECT_FALSE((std::is_move_constructible_v<DisplayLockGuard>));
}

// ---- the rules, checked against the reader sources ---------------------------------------

namespace {

std::vector<std::string> lines(const std::string& path) {
  std::ifstream in(path);
  std::vector<std::string> out;
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

#ifdef CP_REPO_ROOT
const std::string kRoot = CP_REPO_ROOT;
#else
const std::string kRoot = ".";
#endif

}  // namespace

// Every DisplayLockGuard in the reader path is tested before the display is touched: "owns() ==
// false means return immediately" is a rule, so a site that forgets it fails here rather than on
// glass. (C3 item: acquisition failure returns immediately; only the acquirer unlocks.)
TEST(ReaderHandoffSources, EveryGuardIsCheckedBeforeUse) {
  const std::vector<std::string> files = {"src/activities/reader/EpubReaderActivity.cpp",
                                          "src/activities/reader/ClipSelectionActivity.cpp",
                                          "src/activities/reader/DictionaryDefinitionActivity.cpp"};
  int found = 0;
  for (const auto& rel : files) {
    const auto src = lines(kRoot + "/" + rel);
    for (size_t i = 0; i < src.size(); ++i) {
      const auto at = src[i].find("DisplayLockGuard ");
      if (at == std::string::npos) continue;
      if (src[i].find("class DisplayLockGuard") != std::string::npos) continue;
      ++found;
      // "DisplayLockGuard guard;" -> identifier is the token after the type.
      const auto nameStart = src[i].find(' ', at) + 1;
      const auto nameEnd = src[i].find_first_of("; =(", nameStart);
      const std::string name = src[i].substr(nameStart, nameEnd - nameStart);
      bool checked = false;
      for (size_t j = i; j < src.size() && j <= i + 4; ++j) {
        if (src[j].find(name + ".owns()") != std::string::npos) checked = true;
      }
      EXPECT_TRUE(checked) << rel << ":" << (i + 1) << " acquires '" << name << "' and does not check owns()";
    }
  }
  EXPECT_GT(found, 0) << "no DisplayLockGuard sites found: the scan is not looking at the reader path";
}

// The reader path takes the display only through the guard, so a new site cannot acquire it in
// Blocking mode by mistake.
TEST(ReaderHandoffSources, NoRawTryLockRemainsInTheReaderPath) {
  const std::vector<std::string> files = {"src/activities/reader/EpubReaderActivity.cpp",
                                          "src/activities/reader/ClipSelectionActivity.cpp",
                                          "src/activities/reader/DictionaryDefinitionActivity.cpp"};
  for (const auto& rel : files) {
    const auto src = lines(kRoot + "/" + rel);
    for (size_t i = 0; i < src.size(); ++i) {
      EXPECT_EQ(src[i].find("RenderLock::Mode::Try"), std::string::npos)
          << rel << ":" << (i + 1) << " still acquires the display directly; use DisplayLockGuard";
    }
  }
}
