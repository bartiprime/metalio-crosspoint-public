// r163 A2 -- the reader's glyph lookahead: a window of N pages ahead, per-font depth, and the
// idle discipline that keeps it out of a paint and out of a build.
//
// There is no seam under src/ for the reader activity (it owns the panel, the card and the radio),
// so -- like test/poweroff_autooff and test/standby_resume_order -- this is a structural check over
// the production source, plus a negative control in each direction so a green line can never mean
// "the pattern matched nothing".
//
// WHAT IS ASSERTED:
//   L1  the lookahead lives at the no-input tail of loop(), behind `!backgroundBuildTick()`:
//       build first, and the prewarm only on a frame the build did not take;
//   L2  backgroundBuildTick() reports whether it did work (it returned void before this change);
//   L3  idlePrewarmTick() takes the render lock (Try -- never waits for one) and runs the gate
//       predicate under it, then loads exactly ONE page: no loop, so a queued turn is at worst one
//       scan behind;
//   L4  the gate names every competitor: a running build, an open overlay, a scan already in
//       flight, a button/turn pending, the radio, the 32 KiB/16 KiB heap floors, and a partial
//       extension build about to start;
//   L5  the window is bounded: a hard cap, a floor above one page (the old lookahead), and a
//       whole-window time budget;
//   L6  NEGATIVE CONTROL: the pre-change one-shot lookahead (a single page+1 per idle period,
//       with no gate) is gone, and the predicate rejects a synthetic copy of it.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CP_REPO_ROOT
#error "CP_REPO_ROOT must point at the repository root"
#endif

namespace {

std::string readWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Peel // and /* */ comments so a landmark cannot be "found" in prose that describes the code.
std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  size_t i = 0;
  while (i < src.size()) {
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
      i += 2;
      continue;
    }
    out.push_back(src[i]);
    ++i;
  }
  return out;
}

// The body of `Name(...) { ... }` at brace depth 0 of the returned string, or "" if absent.
std::string functionBody(const std::string& src, const std::string& signature) {
  const size_t start = src.find(signature);
  if (start == std::string::npos) return {};
  const size_t open = src.find('{', start + signature.size());
  if (open == std::string::npos) return {};
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    if (src[i] == '}') {
      --depth;
      if (depth == 0) return src.substr(open, i - open + 1);
    }
  }
  return {};
}

size_t countOf(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

class PrewarmLookaheadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cpp_ = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/reader/EpubReaderActivity.cpp"));
    hdr_ = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/reader/EpubReaderActivity.h"));
    ASSERT_FALSE(cpp_.empty()) << "cannot read EpubReaderActivity.cpp";
    ASSERT_FALSE(hdr_.empty()) << "cannot read EpubReaderActivity.h";
  }

  std::string cpp_;
  std::string hdr_;
};

// L1/L2: the build tick answers whether it worked, and the prewarm is the else-branch of it.
TEST_F(PrewarmLookaheadTest, BuildTickRunsFirstAndThePrewarmOnlyWhenItDidNothing) {
  EXPECT_NE(hdr_.find("bool backgroundBuildTick();"), std::string::npos)
      << "backgroundBuildTick() must report whether it did work, or the lookahead cannot yield to it";
  EXPECT_NE(cpp_.find("bool EpubReaderActivity::backgroundBuildTick()"), std::string::npos);

  const auto tick = functionBody(cpp_, "bool EpubReaderActivity::backgroundBuildTick()");
  ASSERT_FALSE(tick.empty());
  EXPECT_EQ(tick.find("return;"), std::string::npos) << "a bool build tick must not return bare";

  const size_t tail = cpp_.find("if (!backgroundBuildTick()) idlePrewarmTick();");
  EXPECT_NE(tail, std::string::npos) << "the lookahead must be the fall-through of the build tick, in one frame";
  // ...and nowhere else: one call site, at the no-input tail.
  EXPECT_EQ(countOf(cpp_, "idlePrewarmTick();"), 1u) << "idlePrewarmTick() belongs at exactly one call site";
}

// L3: the lock is taken with Try (never waits on the render task), the gate runs under it, and a
// single page is loaded per call.
TEST_F(PrewarmLookaheadTest, OnePagePerIdleFrameUnderTheRenderLock) {
  const auto tick = functionBody(cpp_, "void EpubReaderActivity::idlePrewarmTick()");
  ASSERT_FALSE(tick.empty());

  const size_t lock = tick.find("RenderLock lock(RenderLock::Mode::Try);");
  const size_t gate = tick.find("idlePrewarmMayRun()");
  const size_t load = tick.find("section->loadPage(");
  ASSERT_NE(lock, std::string::npos) << "the scan must hold the render lock";
  ASSERT_NE(gate, std::string::npos) << "the lock must be followed by the gate, not by work";
  ASSERT_NE(load, std::string::npos) << "the window advances by loading a page";
  EXPECT_LT(lock, gate);
  EXPECT_LT(gate, load) << "nothing may be loaded before the gate has passed";

  // One page per call: exactly one load, no loop in the body, and nothing that walks several
  // pages (the prewarm of the page itself is the scan below the load).
  EXPECT_EQ(countOf(tick, "loadPage("), 1u) << "one page per idle frame -- a loop here would block a queued turn";
  EXPECT_EQ(tick.find("for ("), std::string::npos);
  EXPECT_EQ(tick.find("while ("), std::string::npos);
}

// L4: every competitor the brief names is in the gate.
TEST_F(PrewarmLookaheadTest, TheGateNamesEveryCompetitor) {
  const auto gate = functionBody(cpp_, "bool EpubReaderActivity::idlePrewarmMayRun() const");
  ASSERT_FALSE(gate.empty());

  for (const char* required :
       {"section->isBuilding()", "overlay != Overlay::None", "hasFrameBuffer()", "isScanning()",
        "buttonContactClosed()", "anyButtonPressed()", "pendingManualTurn != 0", "bleturner::holdsHeap()",
        "BACKGROUND_BUILD_MIN_FREE_HEAP", "BACKGROUND_BUILD_MIN_MAX_ALLOC", "PARTIAL_REBUILD_START_MARGIN",
        "isPartial()"}) {
    EXPECT_NE(gate.find(required), std::string::npos) << "the lookahead gate must check " << required;
  }
}

// L5: bounded. A hard cap, a floor above the old single page, and a whole-window budget.
TEST_F(PrewarmLookaheadTest, TheWindowIsBoundedAndPerFont) {
  EXPECT_NE(hdr_.find("PREWARM_MAX_DEPTH"), std::string::npos) << "a hard cap is required";
  const auto maxDepth = hdr_.find("PREWARM_MAX_DEPTH = ");
  ASSERT_NE(maxDepth, std::string::npos);
  const std::string maxValue = hdr_.substr(maxDepth + std::string("PREWARM_MAX_DEPTH = ").size(), 2);
  EXPECT_EQ(std::stoi(maxValue.substr(0, 1)), 6) << "the cap is six pages: see the header note";

  const auto minDepth = hdr_.find("PREWARM_MIN_DEPTH = ");
  ASSERT_NE(minDepth, std::string::npos);
  const std::string minValue = hdr_.substr(minDepth + std::string("PREWARM_MIN_DEPTH = ").size(), 1);
  EXPECT_GE(std::stoi(minValue), 2) << "the lookahead must reach beyond the single page it used to warm";

  EXPECT_NE(hdr_.find("PREWARM_WINDOW_BUDGET_MS"), std::string::npos)
      << "a slow font may earn the full depth, but the window needs a time budget";
  EXPECT_NE(cpp_.find("prewarmWindowMs >= PREWARM_WINDOW_BUDGET_MS"), std::string::npos);

  // Per font: the depth is derived from the reading font's own measured scan cost, and a font
  // change resets the measurement instead of inheriting another font's number.
  const auto depth = functionBody(cpp_, "int EpubReaderActivity::idlePrewarmDepth() const");
  ASSERT_FALSE(depth.empty());
  EXPECT_NE(depth.find("prewarmFontId != SETTINGS.getReaderFontId()"), std::string::npos);
  EXPECT_NE(depth.find("prewarmAvgMs"), std::string::npos);
  EXPECT_NE(cpp_.find("prewarmAvgMs = 0;"), std::string::npos) << "a new font starts from the floor";
}

// L6: the pre-change one-shot lookahead is gone, and the predicate that described it no longer
// matches -- so a green suite cannot be green because nothing was found.
TEST_F(PrewarmLookaheadTest, TheOneShotLookaheadIsGone) {
  EXPECT_EQ(cpp_.find("idlePrewarmPage"), std::string::npos)
      << "the single-page latch was replaced by the window state (prewarmAnchorPage/prewarmNextPage)";
  EXPECT_EQ(cpp_.find("idlePrewarmSpine"), std::string::npos);
  EXPECT_EQ(cpp_.find("IDLE_PREWARM_DEBOUNCE_MS"), std::string::npos);
  EXPECT_EQ(cpp_.find("const int nextPage = section->currentPage + 1;"), std::string::npos)
      << "the old 'warm exactly page+1' body must be gone";

  // Negative control: the same "one page per idle frame" predicate, run over a synthetic copy of
  // the pre-change block, must fail -- an empty or stale match cannot pass for a window.
  const std::string oldBlock =
      "void EpubReaderActivity::idlePrewarmTick() {\n"
      "  RenderLock lock(RenderLock::Mode::Try);\n"
      "  const int nextPage = section->currentPage + 1;\n"
      "  auto p = section->loadPage(nextPage);\n"
      "}\n";
  const auto oldTick = functionBody(oldBlock, "void EpubReaderActivity::idlePrewarmTick()");
  ASSERT_FALSE(oldTick.empty());
  EXPECT_EQ(countOf(oldTick, "loadPage("), 1u);
  EXPECT_EQ(oldTick.find("idlePrewarmMayRun()"), std::string::npos)
      << "the negative control has no gate: this is exactly why the real body must have one";
}

}  // namespace
