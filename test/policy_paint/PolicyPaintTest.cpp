// POLICY-TESTS — the three paint-policy contracts that keep regressing.
//
// (a) a clean -> DIFFERENT frame must not skip the row-9 completion drive
// (b) a failed completion must drop the diff shadow and arm a from-white retry
// (c) an async paint followed by a FAST paint must not promote to HALF
//
// (c) is decided by a pure function (RefreshPolicy.h) and is asserted directly.
// (a) and (b) are decided inside GfxRenderer::displayBuffer(), which needs the
// panel HAL, PSRAM and a live driver, so they are asserted against the
// production source: the checks read the file and demand the terms the contract
// needs. Both are RED against ba82eb6c -- that is the point of them: they are the
// two paint-policy defects this batch records but may not fix, because the fix
// needs a change under src/ and this batch is forbidden from making one. They are
// labelled `known-red` so a green gate is `ctest -L batch0 -LE known-red`.
//
// The seam the fix will want is written down in docs/planning/BATCH0_GUARD.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "RefreshPolicy.h"

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

// C++ comments and string/char literals stripped: this file is dense with prose
// that names the very identifiers the assertions look for, and with log formats
// that contain braces and quotes.
std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
    } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
      i = std::min(src.size(), i + 2);
    } else if (src[i] == '"' || src[i] == '\'') {
      const char quote = src[i++];
      while (i < src.size() && src[i] != quote) {
        if (src[i] == '\\') ++i;
        ++i;
      }
      if (i < src.size()) ++i;
    } else {
      out.push_back(src[i]);
      ++i;
    }
  }
  return out;
}

size_t matchBrace(const std::string& src, size_t openPos) {
  if (openPos == std::string::npos) return std::string::npos;
  int depth = 0;
  for (size_t i = openPos; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    else if (src[i] == '}') {
      if (--depth == 0) return i;
    }
  }
  return std::string::npos;
}

std::string blockBody(const std::string& src, const std::string& headerNeedle, bool* found = nullptr) {
  const size_t at = src.find(headerNeedle);
  if (found) *found = (at != std::string::npos);
  if (at == std::string::npos) return {};
  const size_t open = src.find('{', at + headerNeedle.size());
  const size_t close = matchBrace(src, open);
  if (open == std::string::npos || close == std::string::npos) return {};
  return src.substr(open + 1, close - open - 1);
}

const std::string kGfxPath = std::string(CP_REPO_ROOT) + "/lib/GfxRenderer/GfxRenderer.cpp";

const std::string& gfxCode() {
  static const std::string code = stripComments(readWholeFile(kGfxPath));
  return code;
}

// True when `name` is actually assigned ( `name = ` and not `name ==` ).
bool assigns(const std::string& body, const std::string& name) {
  const std::string needle = name + " =";
  for (size_t at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1)) {
    const size_t after = at + needle.size();
    if (after >= body.size() || body[after] != '=') return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) clean -> DIFFERENT frame must not skip the row-9 completion.
//
// r96.2 D2 skipped the completion whenever the previous paint was a from-white
// clean, assuming "the same content". If the frame changed between the clean and
// this FAST paint (the common wake-then-scroll case) the skip leaves the
// transition unfinished -- banding. The contract: skip only when the frame is
// demonstrably the one the clean drove (the diff shadow says identical).
// ---------------------------------------------------------------------------
TEST(PolicyTests, KnownRed_SkipCompletionRequiresAFrameIdentityTest) {
  const std::string& code = gfxCode();
  ASSERT_FALSE(code.empty()) << "cannot read " << kGfxPath;

  const size_t skipDef = code.find("skipCompletion =");
  ASSERT_NE(skipDef, std::string::npos) << "the row-9 skip term is gone entirely";
  const size_t skipEnd = code.find(';', skipDef);
  ASSERT_NE(skipEnd, std::string::npos);
  const std::string skipExpr = code.substr(skipDef, skipEnd - skipDef);

  EXPECT_NE(skipExpr.find("sameAsShadow"), std::string::npos)
      << "row-9 completion is skipped on a paint counter alone, with no test that the frame is the "
         "one the clean drove. A clean -> DIFFERENT frame loses its completion drive.\n"
         "  the skip term today is: "
      << skipExpr;
  EXPECT_NE(skipExpr.find("g_paintsSinceClean"), std::string::npos)
      << "the skip term no longer knows a clean happened; the D2 case would regress";
}

// ---------------------------------------------------------------------------
// (b) a FAILED row-9 completion must drop the shadow and arm a from-white retry.
//
// The main drive's failure path already does this (row 12). The completion
// drive's own failure path only resets the latch, so the next paint diffs
// against a shadow describing a frame the glass did not take -- exactly what row
// 12 exists to prevent.
// ---------------------------------------------------------------------------
TEST(PolicyTests, KnownRed_FailedCompletionDropsShadowAndArmsFromWhite) {
  const std::string& code = gfxCode();
  ASSERT_FALSE(code.empty());

  const size_t row9 = code.find("result = lastPanelDrawError();");
  ASSERT_NE(row9, std::string::npos) << "the row-9 completion drive is gone";
  // The failure branch that follows it, up to the end of the row-9 block.
  const size_t branch = code.find("if (result != 0)", row9);
  ASSERT_NE(branch, std::string::npos) << "the completion has no failure branch at all";
  const size_t open = code.find('{', branch);
  const size_t close = matchBrace(code, open);
  ASSERT_NE(close, std::string::npos);
  const std::string body = code.substr(open, close - open);

  EXPECT_NE(body.find("prevSampleValid_ = false"), std::string::npos)
      << "a failed completion leaves the diff shadow valid: the next paint diffs against a frame the "
         "glass never took";
  EXPECT_NE(body.find("promotedRefresh_"), std::string::npos)
      << "a failed completion does not arm the from-white retry the main failure path arms";
  EXPECT_NE(body.find("promotedRefreshPending_ = true"), std::string::npos)
      << "the from-white retry is not actually armed (promotedRefreshPending_ left false)";
}

// ---------------------------------------------------------------------------
// (c) an async paint followed by a FAST paint must not promote to HALF.
//
// Decided purely by cp_refresh_resolve_mode(). A promotion exists to cover a
// from-white FULL that has just landed (row 8) -- the glass holds a full-frame
// waveform and one cheap diff would show it. A diff drive (the async path) has
// no such debt, so the latch it leaves behind must stay "fast" and the next FAST
// paint must stay FAST.
// ---------------------------------------------------------------------------
namespace {

int resolve(int request, bool clean, int lastApplied, bool fullRequest) {
  return cp_refresh_resolve_mode(request, clean ? 1 : 0, lastApplied, fullRequest ? 1 : 0);
}

}  // namespace

TEST(PolicyTests, FastAfterADiffDriveStaysFast) {
  // A diff/async drive leaves CP_LATCH_FAST behind; nothing downstream promotes.
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, false), CP_RM_FAST);
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_CLEAN, false), CP_RM_FAST);
}

TEST(PolicyTests, OnlyAFromWhiteFullPromotesTheNextFastPaint) {
  // ... and that promotion is one-shot: the promoted paint writes FAST.
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FULL, false), CP_RM_HALF);
  EXPECT_EQ(resolve(CP_RM_FAST, false, CP_LATCH_FAST, false), CP_RM_FAST);
}

// The async path must not INVENT a latch from a drive it has not seen finish.
//
// r101.17 (B-P2-3) refined this: displayBufferAsync() may write the latch, but only
// when the drive already ran inside the call -- `!display.supportsAsyncRefresh()`,
// which is true on every panel this fork ships (there is no displayStart, so the
// waveform has completed by the time the call returns). Where a panel really defers,
// supportsAsyncRefresh() is true, there is no result yet, and the latch must be left
// alone -- that is the case this test protects.
TEST(PolicyTests, AsyncPathWritesNoLatch) {
  const std::string& code = gfxCode();
  ASSERT_FALSE(code.empty());

  bool found = false;
  const std::string asyncBody =
      blockBody(code, "void GfxRenderer::displayBufferAsync(HalDisplay::RefreshMode refreshMode)", &found);
  ASSERT_TRUE(found) << "displayBufferAsync() not found";
  ASSERT_FALSE(asyncBody.empty());

  // A latch write is only legitimate behind the "the drive already finished" test.
  if (assigns(asyncBody, "g_lastAppliedMode")) {
    ASSERT_NE(asyncBody.find("supportsAsyncRefresh"), std::string::npos)
        << "displayBufferAsync() writes the applied-mode latch with no supportsAsyncRefresh() guard; on a panel "
           "that really defers the latch would claim a drive result the path never had";
    ASSERT_NE(asyncBody.find("!"), std::string::npos)
        << "the latch write must be conditioned on the drive having finished (!supportsAsyncRefresh())";
  }

  // Arming a promotion is only legitimate as the failure mirror (row 12): the async
  // drive returned an error, the shadow describes content the glass never took, and the
  // next paint must therefore be a from-white FULL. That path also invalidates the latch,
  // so require both or neither.
  if (asyncBody.find("promotedRefreshPending_ = true") != std::string::npos) {
    EXPECT_NE(asyncBody.find("g_lastAppliedValid = false"), std::string::npos)
        << "displayBufferAsync() arms a promotion without invalidating the latch; a promotion is only legitimate as "
           "the row-12 failure mirror where the shadow is declared stale";
  }
  EXPECT_EQ(asyncBody.find("autoRefreshPending_ = true"), std::string::npos)
      << "displayBufferAsync() arms an automatic refresh; the async path must not schedule its own promotion";
}
