#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "activities/reader/ChapterPrefetch.h"

using namespace chapterprefetch;

namespace {

// A frame on which everything holds: the reader is idle in chapter 3 of 20, the paint has
// drained, the next chapter is not cached, and there is room to work.
Gates idleFrame() {
  Gates gates;
  gates.displayOwned = true;
  gates.frameBuffer = true;
  gates.overlayOpen = false;
  gates.overlayRefreshPending = false;
  gates.pageBufferStale = false;
  gates.liveSection = true;
  gates.liveBuilding = false;
  gates.liveNeedsBuild = false;
  gates.scanning = false;
  gates.queuedTurn = false;
  gates.inputDown = false;
  gates.radioHoldsHeap = false;
  gates.freeHeap = kMinFreeHeap + 8 * 1024;
  gates.maxAlloc = kMinMaxAlloc + 8 * 1024;
  return gates;
}

Live inChapter(int spine, uint32_t signature, uint32_t generation, int spineCount = 20) {
  Live live;
  live.spineIndex = spine;
  live.spineCount = spineCount;
  live.layoutSignature = signature;
  live.generation = generation;
  live.layoutKnown = true;
  live.nextSpineExists = spine + 1 < spineCount;
  return live;
}

// A ready artefact for `target`, armed in the session that was live when it was prepared.
State prepared(Target target, uint32_t generation, int pages = 4, uint32_t bytes = 6 * 1024) {
  State state;
  state.target = target;
  state.armedGeneration = generation;
  state.pages = pages;
  state.bytes = bytes;
  state.ready = true;
  return state;
}

}  // namespace

// ---- gates -----------------------------------------------------------------------------------

// Every gate is load-bearing. A test flips one field at a time rather than trusting that the
// condition it was written for is still in the function.
TEST(ChapterPrefetchGates, IdleFrameMayPrepare) {
  EXPECT_TRUE(mayPrepare(idleFrame()));
}

TEST(ChapterPrefetchGates, EveryGateAloneBlocksThePrepare) {
  const std::vector<std::pair<const char*, Gates>> cases = {
      {"display not acquired", [](Gates g) { g.displayOwned = false; return g; }(idleFrame())},
      {"no framebuffer", [](Gates g) { g.frameBuffer = false; return g; }(idleFrame())},
      {"overlay open", [](Gates g) { g.overlayOpen = true; return g; }(idleFrame())},
      {"overlay refresh pending", [](Gates g) { g.overlayRefreshPending = true; return g; }(idleFrame())},
      {"framebuffer stale", [](Gates g) { g.pageBufferStale = true; return g; }(idleFrame())},
      {"no live section", [](Gates g) { g.liveSection = false; return g; }(idleFrame())},
      {"live build running", [](Gates g) { g.liveBuilding = true; return g; }(idleFrame())},
      {"live build about to start", [](Gates g) { g.liveNeedsBuild = true; return g; }(idleFrame())},
      {"glyph scan running", [](Gates g) { g.scanning = true; return g; }(idleFrame())},
      {"turn queued", [](Gates g) { g.queuedTurn = true; return g; }(idleFrame())},
      {"input held", [](Gates g) { g.inputDown = true; return g; }(idleFrame())},
      {"radio holds the heap", [](Gates g) { g.radioHoldsHeap = true; return g; }(idleFrame())},
      {"heap below the free floor", [](Gates g) { g.freeHeap = kMinFreeHeap - 1; return g; }(idleFrame())},
      {"largest block below the floor", [](Gates g) { g.maxAlloc = kMinMaxAlloc - 1; return g; }(idleFrame())},
  };
  for (const auto& [name, gates] : cases) {
    EXPECT_FALSE(mayPrepare(gates)) << name << " did not block the prepare";
  }
}

// Exactly at the floors is admitted: the floors are "at least", not "more than".
TEST(ChapterPrefetchGates, HeapFloorsAreInclusive) {
  Gates gates = idleFrame();
  gates.freeHeap = kMinFreeHeap;
  gates.maxAlloc = kMinMaxAlloc;
  EXPECT_TRUE(mayPrepare(gates));
}

// ---- budgets ---------------------------------------------------------------------------------

// The record ceiling is the page cap times the largest record the section format can hold, so the
// two numbers cannot drift apart.
TEST(ChapterPrefetchBudget, RecordCeilingFollowsThePageCap) {
  EXPECT_EQ(kMaxRecordBytes, static_cast<uint32_t>(kPagesPerAttempt) * 16 * 1024);
  EXPECT_EQ(kMaxSpineBytes, 96u * 1024u);
}

TEST(ChapterPrefetchBudget, WithinBudget) {
  EXPECT_TRUE(withinBudget(1, 1024));
  EXPECT_TRUE(withinBudget(kPagesPerAttempt, kMaxRecordBytes));
  EXPECT_FALSE(withinBudget(kPagesPerAttempt + 1, 1024));  // more pages than one attempt allows
  EXPECT_FALSE(withinBudget(4, kMaxRecordBytes + 1));      // more record bytes than the ceiling
  EXPECT_FALSE(withinBudget(0, 0));                        // nothing laid out is not an artefact
}

TEST(ChapterPrefetchBudget, SpineCapBoundary) {
  EXPECT_FALSE(spineTooBigForPrefetch(kMaxSpineBytes));
  EXPECT_TRUE(spineTooBigForPrefetch(kMaxSpineBytes + 1));
}

// ---- the lifecycle ---------------------------------------------------------------------------

TEST(ChapterPrefetchLifecycle, PreparesWhenEverythingHolds) {
  const State state;
  EXPECT_EQ(nextAction(state, inChapter(3, 0xAA, 7), idleFrame()), Action::Prepare);
}

// Nothing to look ahead to on the last chapter.
TEST(ChapterPrefetchLifecycle, DoesNotPreparePastTheLastChapter) {
  const State state;
  EXPECT_EQ(nextAction(state, inChapter(19, 0xAA, 7, 20), idleFrame()), Action::None);
}

// Before the first paint there is no layout to prepare against.
TEST(ChapterPrefetchLifecycle, DoesNotPrepareBeforeTheFirstLayout) {
  State state;
  Live live = inChapter(3, 0xAA, 7);
  live.layoutKnown = false;
  EXPECT_EQ(nextAction(state, live, idleFrame()), Action::None);
}

// One attempt per chapter: a refusal is remembered against the next spine, not retried every
// idle frame.
TEST(ChapterPrefetchLifecycle, RefusedSpineIsNotRetried) {
  State state;
  state.refusedSpine = 4;
  EXPECT_EQ(nextAction(state, inChapter(3, 0xAA, 7), idleFrame()), Action::None);
  // It expires by itself when the reader moves, because it names a spine, not a chapter.
  EXPECT_EQ(nextAction(state, inChapter(4, 0xAA, 7), idleFrame()), Action::Prepare);
}

// ---- holding, invalidating, adopting ---------------------------------------------------------

// A ready artefact is held across ordinary page turns and repaints: the session generation does
// not move for either, so the prepared chapter survives the reader walking to the end of this one.
TEST(ChapterPrefetchLifecycle, HoldsAcrossPageTurnsInTheSameChapter) {
  const uint32_t generation = 7;
  const State state = prepared({4, 0xAA}, generation);
  for (int page = 0; page < 30; ++page) {
    // A page turn changes nothing this decision reads -- not the spine, not the layout, not the
    // session -- which is exactly why the gate is the generation and not a page count.
    EXPECT_EQ(nextAction(state, inChapter(3, 0xAA, generation), idleFrame()), Action::None);
  }
}

// Chapter jump, TOC, footnote target, search jump, sync apply and a re-open all land here: the
// session generation moved, so the prepared pages describe a layout nobody will paint.
TEST(ChapterPrefetchLifecycle, DiscardsOnSessionChange) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_EQ(nextAction(state, inChapter(3, 0xAA, 8), idleFrame()), Action::Discard);
  // A jump to a different chapter is caught twice over: the generation and the target.
  EXPECT_EQ(nextAction(state, inChapter(12, 0xAA, 8), idleFrame()), Action::Discard);
}

// Font or text-settings change: the layout signature changes under a prepared artefact, and every
// page in it is the wrong shape.
TEST(ChapterPrefetchLifecycle, DiscardsOnLayoutChange) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_EQ(nextAction(state, inChapter(3, 0xBB, 7), idleFrame()), Action::Discard);
}

// Any navigation that leaves this chapter for one that is not the prepared next one drops the
// artefact, even if the session token happens to look unchanged.
TEST(ChapterPrefetchLifecycle, DiscardsWhenTheNextChapterStopsBeingTheTarget) {
  State state = prepared({4, 0xAA}, 7);
  state.armedGeneration = 7;
  EXPECT_EQ(nextAction(state, inChapter(2, 0xAA, 7), idleFrame()), Action::Discard);
}

// The reader walks into the prepared chapter: the render task adopts it.
TEST(ChapterPrefetchLifecycle, AdoptsWhenTheReaderReachesTheTarget) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_EQ(nextAction(state, inChapter(4, 0xAA, 8), idleFrame()), Action::Adopt);
}

// An artefact that never became ready cannot be adopted -- there is nothing on the card.
TEST(ChapterPrefetchLifecycle, NeverAdoptsAnIncompleteArtefact) {
  State state = prepared({4, 0xAA}, 7);
  state.ready = false;
  EXPECT_EQ(nextAction(state, inChapter(4, 0xAA, 8), idleFrame()), Action::Discard);
}

// ---- the adoption decision ------------------------------------------------------------------

TEST(ChapterPrefetchAdoption, AdoptsAForwardTurnIntoAValidTarget) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_TRUE(canAdopt(state, inChapter(4, 0xAA, 8), /*landsOnPageZero=*/true));
}

// A backward chapter turn asks for the chapter's last page (pendingPageJump), which a prepared
// first page cannot answer: the file is dropped and today's path runs unchanged.
TEST(ChapterPrefetchAdoption, RefusesABackwardTurn) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_FALSE(canAdopt(state, inChapter(4, 0xAA, 8), /*landsOnPageZero=*/false));
}

TEST(ChapterPrefetchAdoption, RefusesAChangedLayout) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_FALSE(canAdopt(state, inChapter(4, 0xBB, 8), true));
}

TEST(ChapterPrefetchAdoption, RefusesADifferentChapter) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_FALSE(canAdopt(state, inChapter(5, 0xAA, 8), true));
}

TEST(ChapterPrefetchAdoption, RefusesAnArtefactThatWasNeverReady) {
  State state = prepared({4, 0xAA}, 7);
  state.ready = false;
  EXPECT_FALSE(canAdopt(state, inChapter(4, 0xAA, 8), true));
}

// An adoption is accepted in the same session it was armed in and one generation later: the
// chapter turn is what bumps the token, so the artefact must not be required to match it.
TEST(ChapterPrefetchAdoption, DoesNotRequireTheArmedGeneration) {
  const State state = prepared({4, 0xAA}, 7);
  EXPECT_TRUE(canAdopt(state, inChapter(4, 0xAA, 7), true));
  EXPECT_TRUE(canAdopt(state, inChapter(4, 0xAA, 9), true));
}

// ---- the wiring, checked against the reader source ------------------------------------------

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

size_t firstLineContaining(const std::vector<std::string>& src, const std::string& needle) {
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i].find(needle) != std::string::npos) return i;
  }
  return std::string::npos;
}

}  // namespace

// The prepared section must be a Cache::Prefetch one. A live-cache Section on the next spine would
// write `sections/<n>.bin` before its chapter is open, which is the collision this item exists to
// avoid.
TEST(ChapterPrefetchSources, PreparedSectionUsesItsOwnCachePath) {
  const auto src = lines(kRoot + "/src/activities/reader/EpubReaderActivity.cpp");
  const size_t at = firstLineContaining(src, "Section::Cache::Prefetch");
  ASSERT_NE(at, std::string::npos) << "no prepared Section is constructed with Cache::Prefetch";
  // The prepared section is a different spine: `live.spineIndex + 1`, never the live one.
  EXPECT_NE(src[at].find("next"), std::string::npos) << src[at];
}

// The tick is idle work: it must be reached from the no-input tail of loop(), after the build
// tick, and it must run under the same Try display guard as the build tick -- not from a task.
TEST(ChapterPrefetchSources, PrepareRunsOnTheIdleTail) {
  const auto src = lines(kRoot + "/src/activities/reader/EpubReaderActivity.cpp");
  const size_t tail = firstLineContaining(src, "if (!chapterPrefetchTick()) idlePrewarmTick();");
  ASSERT_NE(tail, std::string::npos) << "the prefetch tick is not on the loop's idle tail";
  // After the build tick, on the same frame that already decided there was no input.
  bool afterBuild = false;
  for (size_t i = tail > 3 ? tail - 3 : 0; i < tail; ++i) {
    if (src[i].find("if (!backgroundBuildTick())") != std::string::npos) afterBuild = true;
  }
  EXPECT_TRUE(afterBuild) << "the prepare must follow the build tick, not precede it";

  const size_t def = firstLineContaining(src, "bool EpubReaderActivity::chapterPrefetchTick()");
  ASSERT_NE(def, std::string::npos) << "chapterPrefetchTick() is not defined";
  bool guard = false;
  bool owned = false;
  for (size_t j = def; j < src.size() && j < def + 14; ++j) {
    if (src[j].find("DisplayLockGuard display;") != std::string::npos) guard = true;
    if (src[j].find("prefetchGates(display.owns())") != std::string::npos) owned = true;
  }
  EXPECT_TRUE(guard) << "chapterPrefetchTick() must acquire the display through DisplayLockGuard";
  EXPECT_TRUE(owned) << "the gates must be read under the lock the work will run under";
}

// r163 A3: every branch of the tick mutates prefetchSection/prefetchState, and the RENDER task
// mutates the same pair in adoptPreparedChapter(). The display lock is the only thing that
// serialises the two, so owning it must be a precondition of the WHOLE function, not just of the
// prepare: Discard/Adopt used to run before any ownership check, which let this task reset a
// unique_ptr the render task was inside.
TEST(ChapterPrefetchSources, TouchesTheArtefactOnlyWhileOwningTheDisplay) {
  const auto src = lines(kRoot + "/src/activities/reader/EpubReaderActivity.cpp");
  const size_t def = firstLineContaining(src, "bool EpubReaderActivity::chapterPrefetchTick()");
  ASSERT_NE(def, std::string::npos);
  size_t owns = std::string::npos;
  size_t switchAt = std::string::npos;
  size_t live = std::string::npos;
  for (size_t j = def; j < src.size() && j < def + 20; ++j) {
    if (owns == std::string::npos && src[j].find("if (!display.owns()) return false;") != std::string::npos) owns = j;
    if (switchAt == std::string::npos && src[j].find("nextAction(prefetchState") != std::string::npos) switchAt = j;
    if (live == std::string::npos && src[j].find("prefetchLive()") != std::string::npos) live = j;
  }
  ASSERT_NE(owns, std::string::npos) << "the tick must refuse to run without the display lock";
  ASSERT_NE(switchAt, std::string::npos);
  EXPECT_LT(owns, switchAt) << "the ownership check must come before any Discard/Adopt branch";
  if (live != std::string::npos) EXPECT_LT(owns, live);
}

// Adoption happens before the live Section is created, or it cannot be an adoption at all.
TEST(ChapterPrefetchSources, AdoptsBeforeTheLiveSectionIsCreated) {
  const auto src = lines(kRoot + "/src/activities/reader/EpubReaderActivity.cpp");
  const size_t open = firstLineContaining(src, "new Section(epub, currentSpineIndex, renderer)");
  ASSERT_NE(open, std::string::npos);
  bool adoptBefore = false;
  for (size_t i = open > 6 ? open - 6 : 0; i < open; ++i) {
    if (src[i].find("adoptPreparedChapter()") != std::string::npos) adoptBefore = true;
  }
  EXPECT_TRUE(adoptBefore) << "adoptPreparedChapter() must run before the live Section is built";
}

// Sleep and exit drop the artefact. A prepared chapter must not outlive the visit that made it.
TEST(ChapterPrefetchSources, SleepAndExitDiscardTheArtefact) {
  const auto src = lines(kRoot + "/src/activities/reader/EpubReaderActivity.cpp");
  const size_t sleep = firstLineContaining(src, "void EpubReaderActivity::prepareForSleep()");
  ASSERT_NE(sleep, std::string::npos);
  bool discarded = false;
  for (size_t i = sleep; i < src.size() && i < sleep + 12; ++i) {
    if (src[i].find("discardPreparedChapter()") != std::string::npos) discarded = true;
  }
  EXPECT_TRUE(discarded) << "prepareForSleep() must discard the prepared chapter";
}
