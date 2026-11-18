// ---------------------------------------------------------------------------
// EOB1 call-site check — the end-of-book branch of ReaderActivity::render()
// must not clear the canvas unconditionally, and its drive must come from the
// pure table, not from an intuition.
//
// WHY A SOURCE CHECK: ReaderActivity has no seam under src/ (it is an abstract
// activity over two concrete readers) and this batch may not add one, so the
// premise -- "the clear is the cost driver" -- can only be pinned at the source.
// Same technique as test/poweroff_autooff and test/standby_resume_order.
//
// WHAT IS ASSERTED, over the `if (isAtEndOfBook())` branch only:
//   L1  the branch consults eobpanel::plan();
//   L2  EVERY clearScreen() in the branch is gated on plan.clearScreen -- the
//       pre-fix `renderer.clearScreen();` (unconditional, on entry AND on every
//       selection repaint) fails L2;
//   L3  the branch arms the plan's pins (promoteFast / suppressCompletion /
//       binaryFast) or promoteFull, so the resolved drive is the table's;
//   L4  NEGATIVE CONTROL: the verbatim pre-fix branch text must FAIL the same
//       predicate, so a green L2 can never mean "the predicate matches nothing";
//   L5  SCOPE: the rest of the file (the page-turn paint) must not have gained
//       a plan() call -- the reader's own refresh cadence is out of scope.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

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

size_t countOf(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
  return n;
}

// The `if (isAtEndOfBook()) {` branch of ReaderActivity::render(): everything up
// to the reader's own paint.
std::string eobBranch(const std::string& source) {
  const std::string mark = "if (isAtEndOfBook()) {";
  const size_t at = source.find(mark);
  if (at == std::string::npos) return {};
  const size_t end = source.find("  renderBook();", at);
  if (end == std::string::npos) return {};
  return source.substr(at, end - at);
}

// The claim. True = the branch clears unconditionally (the pre-fix behaviour).
bool clearsUnconditionally(const std::string& branch) {
  if (branch.empty()) return true;  // cannot prove anything: treat as broken
  if (countOf(branch, "eobpanel::plan(") == 0) return true;  // no table, no decision
  if (countOf(branch, "promoteFull") == 0 && countOf(branch, "promoteFast") == 0) return true;
  // Every call that CLEARS must be on a line that also names the plan's term.
  // The pre-fix `renderer.clearScreen();` is a line of its own and fails here.
  size_t clears = 0;
  size_t gated = 0;
  size_t at = branch.find("renderer.clearScreen");
  while (at != std::string::npos) {
    ++clears;
    const size_t lineStart = branch.rfind('\n', at);
    const size_t lineEnd = branch.find('\n', at);
    const std::string line = branch.substr(lineStart == std::string::npos ? 0 : lineStart,
                                           (lineEnd == std::string::npos ? branch.size() : lineEnd) -
                                               (lineStart == std::string::npos ? 0 : lineStart));
    if (line.find("plan.clearScreen") != std::string::npos) ++gated;
    at = branch.find("renderer.clearScreen", at + 1);
  }
  if (clears == 0) return true;     // a panel that never clears on entry...
  if (gated < clears) return true;  // ...or one that clears without the plan saying so
  return false;
}

// Verbatim from commit 6a5a4ef6 (ReaderActivity::render(), the branch under test).
const char* kPreFixBranch =
    "  if (isAtEndOfBook()) {\n"
    "    if (!endOfBookOptions) {\n"
    "      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);\n"
    "      if (!endOfBookOptions) LOG_ERR(\"READER\", \"OOM: EndOfBookOptions\");\n"
    "    }\n"
    "    renderer.clearScreen();\n"
    "    if (endOfBookOptions) {\n"
    "      endOfBookOptions->loadOnce(bookPath);\n"
    "      endOfBookOptionsReady.store(true, std::memory_order_release);\n"
    "      endOfBookOptions->render(renderer, mappedInput);\n"
    "    }\n"
    "    renderer.displayBuffer();\n"
    "    onEndOfBookRendered();\n"
    "    markPageRendered();\n"
    "    readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());\n"
    "    return;\n"
    "  }\n";

}  // namespace

TEST(Eob1CallSite, TheEndOfBookBranchDoesNotClearUnconditionally) {
  const std::string source = readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/reader/ReaderActivity.cpp");
  ASSERT_FALSE(source.empty()) << "cannot read ReaderActivity.cpp -- the check would pass vacuously";
  const std::string branch = eobBranch(source);
  ASSERT_FALSE(branch.empty()) << "the isAtEndOfBook() branch was not located";
  EXPECT_FALSE(clearsUnconditionally(branch))
      << "the EOB branch must gate its clearScreen() on plan.clearScreen and consult eobpanel::plan()";
}

TEST(Eob1CallSite, TheNegativeControlFailsTheSamePredicate) {
  // L4: the pre-fix text is what this check exists to catch. If this ever
  // passes, the predicate is broken, not the product.
  EXPECT_TRUE(clearsUnconditionally(std::string(kPreFixBranch)))
      << "the pre-fix branch (bare renderer.clearScreen() on every render) must be caught";
  EXPECT_EQ(countOf(std::string(kPreFixBranch), "eobpanel::plan("), 0u);
}

TEST(Eob1CallSite, ThePageTurnPaintKeptItsOwnCadence) {
  // L5: one plan() call site, inside the EOB branch. The reader's page paint
  // (renderBook()) must not have grown one.
  const std::string source = readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/reader/ReaderActivity.cpp");
  ASSERT_FALSE(source.empty());
  const std::string branch = eobBranch(source);
  EXPECT_EQ(countOf(source, "eobpanel::plan("), countOf(branch, "eobpanel::plan("))
      << "plan() must appear only in the end-of-book branch";
}
