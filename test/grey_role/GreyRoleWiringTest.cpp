// r163 D3 -- the WIRING of the role tag, as text over the production sources.
//
// The two claims that must hold for this to be an ADMISSION rule and nothing more:
//
//   G1  every writer that can put an intermediate level on the canvas asks the ONE
//       predicate (greyscaleAdmitted()), so the table cannot be bypassed by a path
//       that forgot it;
//   G2  the tag is scoped -- a tagged draw restores the previous role, so a cover tag
//       cannot leak into the next element on the same paint;
//   G3  no role reaches a refresh-mode decision: RefreshPolicy and the waveform
//       tables do not name it. The lane is chosen exactly as before.

#include <gtest/gtest.h>

#include <algorithm>
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
    } else if (src[i] == '\r') {
      ++i;
    } else {
      out.push_back(src[i]);
      ++i;
    }
  }
  return out;
}

TEST(GreyRoleWiring, EveryLevelWriterAsksTheOnePredicate) {
  const std::string src = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/lib/GfxRenderer/GfxRenderer.cpp"));
  ASSERT_FALSE(src.empty());
  // The explicit-level choke point (commitPixelPhysical).
  EXPECT_NE(src.find("const uint8_t written = greyscaleAdmitted()"), std::string::npos);
  // The glyph blit target (drawGlyphBitmap).
  EXPECT_NE(src.find("target.binary = !greyscaleAdmitted();"), std::string::npos);
  // The scaled-codepoint path (drop caps).
  EXPECT_NE(src.find("greyscaleAdmitted() ? pixfmt::glyphLevelToNibble"), std::string::npos);
  // ...and none of them reads textBinary_ directly any more: one predicate, or the
  // role could be skipped by whichever writer was edited last.
  EXPECT_EQ(src.find("written = textBinary_"), std::string::npos);
  EXPECT_EQ(src.find("target.binary = textBinary_"), std::string::npos);
}

TEST(GreyRoleWiring, TheTagIsScopedAndRestoresOnEveryPathOut) {
  const std::string header = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/lib/GfxRenderer/GfxRenderer.h"));
  ASSERT_FALSE(header.empty());
  EXPECT_NE(header.find("class ScopedContentRole"), std::string::npos);
  EXPECT_NE(header.find("~ScopedContentRole() { r_.setContentRole(previous_); }"), std::string::npos);
  // One function decides admission for the whole renderer.
  EXPECT_NE(header.find("bool greyscaleAdmitted() const { return !textBinary_ && roleAdmitsGrey(contentRole_); }"),
            std::string::npos);
}

TEST(GreyRoleWiring, IconTaggedCallSitesAreTheGlyphAndTheStatusBar) {
  const std::string renderer = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/lib/GfxRenderer/GfxRenderer.cpp"));
  ASSERT_FALSE(renderer.empty());
  EXPECT_NE(renderer.find("ContentRole::Icon"), std::string::npos) << "drawIcon() itself must be tagged";

  const std::string theme = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/components/themes/BaseTheme.cpp"));
  ASSERT_FALSE(theme.empty());
  EXPECT_NE(theme.find("drawBatteryOutline(const GfxRenderer& renderer"), std::string::npos);
  EXPECT_NE(theme.find("ContentRole::Cover"), std::string::npos) << "the cover slot is the one visible-content tag";
}

TEST(GreyRoleWiring, NoRoleReachesTheRefreshDecision) {
  // The lane is RefreshPolicy's, and it must not know a content role exists. This is
  // the check that keeps "admission" from drifting into "which waveform".
  const std::string policy =
      stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/display/RefreshPolicy.h"));
  ASSERT_FALSE(policy.empty());
  EXPECT_EQ(policy.find("ContentRole"), std::string::npos) << "RefreshPolicy must not consult a content role";
  EXPECT_EQ(policy.find("contentRole"), std::string::npos);

  // And inside the renderer, the drive path (displayBuffer / displayBufferAsync) reads
  // the BRIGHTNESS request it always read, never the role.
  const std::string renderer = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/lib/GfxRenderer/GfxRenderer.cpp"));
  ASSERT_FALSE(renderer.empty());
  const size_t drive = renderer.find("void GfxRenderer::displayBuffer(");
  ASSERT_NE(drive, std::string::npos);
  const size_t next = renderer.find("void GfxRenderer::displayBufferAsync(", drive);
  ASSERT_NE(next, std::string::npos);
  EXPECT_EQ(renderer.substr(drive, next - drive).find("contentRole"), std::string::npos)
      << "the drive must not be conditioned on a content role";
}

}  // namespace
