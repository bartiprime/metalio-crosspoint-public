// SET3 (r164), the WIRING -- as text over the production sources.
//
// SettingsActivity is not host-compilable (it pulls the whole UI stack), so the
// behavioural half lives in SettingsSelectorTest.cpp over the pure decision and
// the SDK's real ListNav. What is left is the set of claims no type can make:
//
//   W1  a discrete row opens the option list and writes NOTHING on the way out;
//   W2  the chosen index is what gets persisted, in the same slot, through the
//       same save call -- and a cancelled list writes nothing at all;
//   W3  the selector reports the chosen index as a result and a Back as a cancel;
//   W4  the checkbox drawing and the checkbox activation read the same decision;
//   W5  the pop back to the parent does not re-run onEnter(), and onEnter() is
//       the only caller of activeNav().reset() -- which is what makes the scroll
//       position survive (SettingsSelectorScroll proves the nav's own behaviour);
//   W6  the selector opts into the theme's list chrome, so it inherits Andro /
//       PickyOS row treatment instead of drawing its own.

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

const std::string kSettings = stripComments(readWholeFile(std::string(CP_REPO_ROOT) +
                                                          "/src/activities/settings/SettingsActivity.cpp"));
const std::string kSelector = stripComments(readWholeFile(std::string(CP_REPO_ROOT) +
                                                          "/src/activities/settings/OptionSelectActivity.cpp"));
const std::string kManager = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/ActivityManager.cpp"));
const std::string kList = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/UiListActivity.cpp"));

// The body of a function, from its opening brace to the matching one.
std::string blockBody(const std::string& src, const std::string& signature) {
  const size_t sig = src.find(signature);
  if (sig == std::string::npos) return {};
  const size_t open = src.find('{', sig);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') depth++;
    if (src[i] == '}') {
      depth--;
      if (depth == 0) return src.substr(open, i - open + 1);
    }
  }
  return {};
}

}  // namespace

TEST(SettingsSelectorWiring, SourcesExist) {
  ASSERT_FALSE(kSettings.empty()) << "SettingsActivity.cpp not found under CP_REPO_ROOT";
  ASSERT_FALSE(kSelector.empty()) << "OptionSelectActivity.cpp not found under CP_REPO_ROOT";
  ASSERT_FALSE(kManager.empty());
  ASSERT_FALSE(kList.empty());
}

TEST(SettingsSelectorWiring, W1_DiscreteRowOpensTheListAndWritesNothing) {
  const std::string body = blockBody(kSettings, "void SettingsActivity::toggleSelectedSetting()");
  ASSERT_FALSE(body.empty());

  const size_t selector = body.find("settingsrow::Kind::Selector");
  const size_t open = body.find("openOptionSelector(setting)");
  ASSERT_NE(selector, std::string::npos) << "the activation path does not consult the row kind";
  ASSERT_NE(open, std::string::npos) << "no selector is opened for a discrete row";
  EXPECT_LT(selector, open) << "the selector branch must be decided before anything is opened";

  // Between the branch and its return there must be no value write: the row is
  // applied from the list's result, not on the way in.
  const size_t branchEnd = body.find("}", open);
  ASSERT_NE(branchEnd, std::string::npos);
  const std::string branch = body.substr(selector, branchEnd - selector);
  EXPECT_EQ(branch.find("saveToFile"), std::string::npos) << "the selector branch persists before a choice exists";
  EXPECT_EQ(branch.find("= static_cast<uint8_t>"), std::string::npos);
}

TEST(SettingsSelectorWiring, W2_ChosenIndexIsWhatGetsPersisted) {
  const std::string body = blockBody(kSettings, "void SettingsActivity::openOptionSelector");
  ASSERT_FALSE(body.empty()) << "openOptionSelector not found";

  // Cancel first: the early return must precede every write in the handler.
  const size_t cancel = body.find("if (result.isCancelled) return;");
  const size_t chosen = body.find("std::get_if<OptionIndexResult>");
  const size_t write = body.find("SETTINGS.*valuePtr = static_cast<uint8_t>(chosen->index)");
  const size_t setter = body.find("setter(static_cast<uint8_t>(chosen->index))");
  const size_t save = body.find("SETTINGS.saveToFile()");
  ASSERT_NE(cancel, std::string::npos) << "a cancelled selector would still write";
  ASSERT_NE(chosen, std::string::npos);
  ASSERT_NE(write, std::string::npos) << "the chosen index is not written to the row's own slot";
  ASSERT_NE(setter, std::string::npos) << "a dynamic row is not applied through its own setter";
  ASSERT_NE(save, std::string::npos) << "the choice is never persisted";
  EXPECT_LT(cancel, chosen) << "the write comes before the cancel test";
  EXPECT_LT(write, save) << "saveToFile() must follow the write, as the cycling path did";
  EXPECT_LT(setter, save);

  // Same call the in-place step ends with: one persistence rule for both.
  const std::string toggle = blockBody(kSettings, "void SettingsActivity::toggleSelectedSetting()");
  EXPECT_NE(toggle.find("SETTINGS.saveToFile()"), std::string::npos);
}

TEST(SettingsSelectorWiring, W3_SelectorReportsAnIndexAndCancelsOnBack) {
  const std::string activate = blockBody(kSelector, "void OptionSelectActivity::activateIndex");
  ASSERT_FALSE(activate.empty());
  EXPECT_NE(activate.find("OptionIndexResult{index}"), std::string::npos)
      << "the list does not report which option was chosen";
  EXPECT_NE(activate.find("finish()"), std::string::npos) << "the list does not close on selection";
  EXPECT_EQ(activate.find("saveToFile"), std::string::npos) << "the list must not persist; the caller owns the setting";

  const std::string back = blockBody(kSelector, "void OptionSelectActivity::onBackButton");
  ASSERT_FALSE(back.empty()) << "Back is not handled -- it would inherit a non-cancelling finish()";
  EXPECT_NE(back.find("isCancelled = true"), std::string::npos) << "Back must report a cancel";
  EXPECT_NE(back.find("finish()"), std::string::npos);
}

TEST(SettingsSelectorWiring, W4_RenderAndActivationShareOneDecision) {
  const std::string build = blockBody(kSettings, "void SettingsActivity::buildScreen");
  ASSERT_FALSE(build.empty());
  EXPECT_NE(build.find("settingKind(setting) == settingsrow::Kind::Checkbox"), std::string::npos)
      << "the renderer re-derives the checkbox rule instead of asking the row kind";
  // The old duplicate test (the OFF/ON label scan) must be gone from the renderer.
  EXPECT_EQ(build.find("STR_STATE_OFF"), std::string::npos)
      << "buildScreen still carries its own OFF/ON scan; two rules can disagree";
}

TEST(SettingsSelectorWiring, W5_PopDoesNotReRunOnEnterSoTheViewportSurvives) {
  // The restored-parent branch: from the pop to the next loop iteration.
  const size_t pop = kManager.find("Popped from activity stack");
  ASSERT_NE(pop, std::string::npos);
  const size_t branchEnd = kManager.find("continue;", pop);
  ASSERT_NE(branchEnd, std::string::npos);
  const std::string branch = kManager.substr(pop, branchEnd - pop);
  EXPECT_EQ(branch.find("onEnter"), std::string::npos)
      << "the pop path re-enters the parent; onEnter() would reset the list nav and the scroll position";

  // ...and reset() really is reachable only from onEnter(), which is what makes
  // the absence above sufficient.
  const std::string onEnter = blockBody(kList, "void UiListActivity::onEnter()");
  ASSERT_FALSE(onEnter.empty());
  EXPECT_NE(onEnter.find("activeNav().reset()"), std::string::npos);

  size_t occurrences = 0;
  for (size_t at = kList.find("activeNav().reset()"); at != std::string::npos;
       at = kList.find("activeNav().reset()", at + 1)) {
    occurrences++;
  }
  EXPECT_EQ(occurrences, 1u) << "the nav is reset somewhere other than onEnter(); the scroll-position argument no longer holds";

  // The settings screen itself must not reset its nav in the result handler.
  const std::string selectorBody = blockBody(kSettings, "void SettingsActivity::openOptionSelector");
  EXPECT_EQ(selectorBody.find("nav.reset"), std::string::npos);
  EXPECT_EQ(selectorBody.find("nav.top"), std::string::npos);
  EXPECT_EQ(selectorBody.find("nav.selected"), std::string::npos);
}

TEST(SettingsSelectorWiring, W6_SelectorInheritsTheThemeChrome) {
  const std::string header = stripComments(
      readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/settings/OptionSelectActivity.h"));
  ASSERT_FALSE(header.empty());
  EXPECT_NE(header.find("usesThemeListChrome() const override { return true; }"), std::string::npos)
      << "the selector would draw its own list chrome instead of the theme's";
  EXPECT_NE(header.find("usesListFooterPager() const override { return true; }"), std::string::npos)
      << "a long option list would have no pager on a paging theme";
}
