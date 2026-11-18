// r163 E3 -- WHICH CHARGE FLAG DOES EACH CONSUMER USE?
//
// The truth table has two answers and they are different on purpose:
//
//   isCharging()            RAW   -- current > +5 mA with a +-5 mA deadband.
//   isChargingDisplayed()   SHOWN -- raw && displayed SoC < 100.
//
// A user-visible bolt must use the SHOWN one (no lightning while the pack sits full in
// the CV tail); a decision must use the RAW one (a charge session is still a charge
// session at 100 %). A consumer that picks the wrong one is not a compile error, so this
// check reads the production sources. There is no seam under src/ for the themes and this
// batch may not add one -- same technique as test/standby_resume_order and test/aud_gate.
//
// Every assertion is on COMMENT-STRIPPED text, so a comment that merely names the call
// cannot satisfy it.

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

// Peel `//` and `/* */` comments, keeping line structure.
std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  bool line = false;
  bool block = false;
  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
    if (line) {
      if (c == '\n') {
        line = false;
        out.push_back(c);
      }
      continue;
    }
    if (block) {
      if (c == '*' && n == '/') {
        block = false;
        ++i;
      } else if (c == '\n') {
        out.push_back(c);
      }
      continue;
    }
    if (c == '/' && n == '/') {
      line = true;
      ++i;
      continue;
    }
    if (c == '/' && n == '*') {
      block = true;
      ++i;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

size_t countOf(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

std::string source(const char* relative) {
  return stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/" + relative));
}

// The raw accessor as a token: "isCharging()" must not be counted inside
// "isChargingDisplayed()", whose prefix it is.
constexpr const char* kRaw = "isCharging()";
constexpr const char* kShown = "isChargingDisplayed()";

}  // namespace

// The status bar's bolt, both themes, and the fui header's status struct.
TEST(BatteryGaugeWiring, ThemeBoltUsesTheDisplayedFlag) {
  const std::string base = source("src/components/themes/BaseTheme.cpp");
  ASSERT_FALSE(base.empty());
  EXPECT_GE(countOf(base, kShown), 2u) << "BaseTheme draws the bolt (fillBatteryIcon) and fills status.battery.charging";
  EXPECT_EQ(countOf(base, kRaw), 0u) << "a user-visible bolt must not use the RAW flag";

  const std::string lyra = source("src/components/themes/lyra/LyraTheme.cpp");
  ASSERT_FALSE(lyra.empty());
  EXPECT_EQ(countOf(lyra, kShown), 1u);
  EXPECT_EQ(countOf(lyra, kRaw), 0u);
}

// The partial-repaint path: the claim is what the frame DRAWS, and its edge detector must
// watch the same value, or a repaint is scheduled for a state the glass never showed.
TEST(BatteryGaugeWiring, IconRepaintWatchesTheDisplayedFlag) {
  const std::string icon = source("src/display/BatteryIconRegion.cpp");
  ASSERT_FALSE(icon.empty());
  EXPECT_EQ(countOf(icon, kShown), 2u) << "the paint claim and cpBatteryIconPollAndRepaint's edge";
  EXPECT_EQ(countOf(icon, kRaw), 0u);
}

// The stats record is a DECISION: the CV tail is still a charge session.
TEST(BatteryGaugeWiring, BatteryStatsKeepsTheRawFlag) {
  const std::string stats = source("src/util/BatteryStats.cpp");
  ASSERT_FALSE(stats.empty());
  EXPECT_GE(countOf(stats, "chargingForStats()"), 1u);
  EXPECT_EQ(countOf(stats, kShown), 0u) << "the record must not follow the icon";
}

// Both answers exist, on the same object, with the raw one still the cached bool.
TEST(BatteryGaugeWiring, HalPowerManagerExposesBoth) {
  const std::string hdr = source("lib/hal/HalPowerManager.h");
  ASSERT_FALSE(hdr.empty());
  EXPECT_GE(countOf(hdr, "bool isCharging() const;"), 1u);
  EXPECT_GE(countOf(hdr, "bool isChargingDisplayed() const;"), 1u);
}

// r163 E2: the standby resume must tell the gauge machines, and it must do so AFTER the
// wake-time recorder (rails/bus/card are back by then) and BEFORE anything samples or
// paints. A missing call is silent: the confirmation and the filter would simply never
// re-validate, which looks like a slow gauge rather than a bug.
TEST(BatteryGaugeWiring, StandbyResumeNotifiesTheGaugeMachines) {
  const std::string main = source("src/main.cpp");
  ASSERT_FALSE(main.empty());
  const size_t wake = main.find("powerManager.notifyStandbyWake();");
  ASSERT_NE(wake, std::string::npos) << "the resume never tells the gauge it woke";
  const size_t wakerec = main.find("wakerecResume();");
  ASSERT_NE(wakerec, std::string::npos);
  EXPECT_LT(wakerec, wake) << "notify after the wake-time recorder";

  // The FIRST display.wake() after the recorder is the paint gate re-open; the hook must
  // land before it so the settle clock starts on the real resume edge.
  const size_t displayWake = main.find("display.wake();", wake);
  ASSERT_NE(displayWake, std::string::npos) << "expected the paint gate re-open after the hook";
  EXPECT_LT(wake, displayWake);
}

// r163 E1: the driver must stay a SAMPLE SOURCE -- the percentage the UI gets comes out of
// the machine, not out of the register read.
TEST(BatteryGaugeWiring, DisplayedSocComesFromTheMachine) {
  const std::string pm = source("lib/hal/HalPowerManager.cpp");
  ASSERT_FALSE(pm.empty());
  EXPECT_GE(countOf(pm, "_socMachine.sample("), 1u);
  EXPECT_GE(countOf(pm, "_socMachine.displayed()"), 1u) << "the cached percent is the machine's";
  EXPECT_GE(countOf(pm, "_fullCharge.sample("), 1u);
}

// NEGATIVE CONTROL: the predicate must be able to fail. A synthetic file that calls the
// raw flag where the theme should call the displayed one is rejected, so a green run above
// cannot mean "the search matched nothing".
TEST(BatteryGaugeWiring, PredicateRejectsTheWrongCall) {
  const std::string wrong = stripComments("void f() { const bool c = powerManager.isCharging(); }");
  EXPECT_EQ(countOf(wrong, kRaw), 1u);
  EXPECT_EQ(countOf(wrong, kShown), 0u);

  const std::string commentedOut = stripComments("// powerManager.isChargingDisplayed()\nint x;");
  EXPECT_EQ(countOf(commentedOut, kShown), 0u) << "a mention in a comment is not a call";
}
