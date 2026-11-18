// ---------------------------------------------------------------------------
// EXP1 host test — the arm-defaults gate (r101.21).
//
// This file exists to make ONE claim falsifiable on the host, without a device:
//
//   every arm's compiled default is the behaviour r101.20 already had,
//   and a fresh boot reports every arm at that default.
//
// That is the failure mode the batch is most exposed to (plan §7 #1): someone
// writes `bool row9 = false` meaning "the new thing" and the label "disarmed"
// quietly ships a behaviour change. `todayValue()` is written down separately from
// `buildDefault()`, so flipping either one fails here instead of on the glass.
//
// It also checks the status format, because `exp status` is the device session's
// FIRST command and the check that "any arm reading on means re-flash".
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "platform/ExpArms.h"

namespace {

// A "fresh boot": the static initialisers are the boot state, and this process has
// just been started. Nothing in this test may persist an arm.
constexpr size_t kArmCount = static_cast<size_t>(exparms::Arm::Count);

std::string statusText() {
  char buf[1024];
  const size_t n = exparms::status(buf, sizeof(buf));
  return std::string(buf, n);
}

}  // namespace

// The ONE way an arm may ship with a default that is NOT r101.20's behaviour.
// The name has to be listed here deliberately, with its argument at the
// definition (src/platform/ExpArms.cpp), so an accidental flip still fails this
// test while an operator-requested fix remains shippable. `exp <name> off`
// restores r101.20's behaviour in RAM for the session.
const char* const kShippedArmed[] = {
    // FLP1 (r101.21b): the operator reported that every frontlight adjustment
    // flashed the whole panel, and asked for the fix in this image.
    "flpanel",
};

bool shipsArmed(const char* armName) {
  for (const char* n : kShippedArmed) {
    if (std::strcmp(n, armName) == 0) return true;
  }
  return false;
}

// Every arm, by index — the table is append-only, so this loop is the whole list.
TEST(ExpArms, EveryArmDefaultsToTodaysBehaviourUnlessItIsDeclaredShippedArmed) {
  for (size_t i = 0; i < kArmCount; ++i) {
    const auto a = static_cast<exparms::Arm>(i);
    if (shipsArmed(exparms::name(a))) {
      EXPECT_TRUE(exparms::buildDefault(a))
          << "arm '" << exparms::name(a) << "' is declared shipped-armed but its compiled default is off";
      EXPECT_FALSE(exparms::todayValue(a))
          << "arm '" << exparms::name(a) << "' is declared shipped-armed because r101.20 did NOT do this";
      continue;
    }
    EXPECT_EQ(exparms::buildDefault(a), exparms::todayValue(a))
        << "arm '" << exparms::name(a) << "' (#" << i
        << ") has a compiled default that differs from r101.20's behaviour";
  }
}

// The frozen values, spelled out. If this test needs editing, the batch changed
// behaviour and the identity proof in the plan is void.
TEST(ExpArms, TheFrozenValuesAreTheOnesR10120ShippedWith) {
  EXPECT_TRUE(exparms::todayValue(exparms::Arm::Row9Completion))
      << "r101.20 appended the GL16 completion drive after a FAST paint";
  EXPECT_FALSE(exparms::todayValue(exparms::Arm::VidShortDu))
      << "r101.20 never selected the 8-phase short-DU table";
  // FLP1: r101.20 flapped the whole panel on every adjustment -- no explicit
  // FAST, no suppressed completion, no clean on exit.
  EXPECT_FALSE(exparms::todayValue(exparms::Arm::FrontlightPanelPartial))
      << "r101.20 did not give the frontlight panel a one-drive adjustment";
  EXPECT_TRUE(exparms::todayValue(exparms::Arm::Row9Completion)
              && exparms::buildDefault(exparms::Arm::Row9Completion))
      << "the row-9 arm itself still ships at r101.20's behaviour";
}

// A fresh boot reports every arm at its default — i.e. `exp status` on a device
// that has not been typed at describes r101.20, not the batch.
TEST(ExpArms, FreshBootReportsEveryArmAtItsDefault) {
  for (size_t i = 0; i < kArmCount; ++i) {
    const auto a = static_cast<exparms::Arm>(i);
    EXPECT_EQ(exparms::effective(a), exparms::buildDefault(a))
        << "arm '" << exparms::name(a) << "' is not at its build default in a fresh process";
  }
  // The one consequence the device session checks by eye: row9 must report ON.
  EXPECT_TRUE(exparms::completionDrive()) << "row-9 completion drive is suppressed in a fresh boot";
  EXPECT_FALSE(exparms::vidShortDu()) << "the short-DU table is selected in a fresh boot";
  EXPECT_TRUE(exparms::panelPartialRefresh()) << "FLP1 ships armed (the operator's fix)";
}

// `exp status` is one line per arm, each starting with the arm's own name, and the
// line count equals the arm count — so `grep -c '^exp '` on the device is a count.
TEST(ExpArms, StatusIsOneGreppableLinePerArm) {
  const std::string s = statusText();
  size_t lines = 0;
  size_t start = 0;
  while (start < s.size()) {
    const size_t nl = s.find('\n', start);
    const std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.rfind("exp ", 0), 0u) << "line does not start with 'exp ': " << line;
    EXPECT_NE(line.find("default="), std::string::npos) << "line has no default: " << line;
    // The line must name an arm from the table, not a typo of one.
    bool named = false;
    for (size_t i = 0; i < kArmCount; ++i) {
      const std::string prefix = std::string("exp ") + exparms::name(static_cast<exparms::Arm>(i)) +
                                 "=";
      if (line.rfind(prefix, 0) == 0) named = true;
    }
    EXPECT_TRUE(named) << "line names no arm: " << line;
    ++lines;
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  EXPECT_EQ(lines, kArmCount) << "status must print exactly one line per arm";
  // And the shipped image's first device command must read as r101.20: row9=on.
  EXPECT_NE(s.find("exp row9=on default=on"), std::string::npos)
      << "a fresh image must report row9=on; got:\n"
      << s;
  EXPECT_NE(s.find("exp vid=off default=off"), std::string::npos)
      << "a fresh image must report vid=off; got:\n"
      << s;
  // The shipped-armed one, documented in `exp status` so a device session can
  // see the panel fix is ON and turn it off from the shell.
  EXPECT_NE(s.find("exp flpanel=on default=on"), std::string::npos)
      << "a fresh image must report flpanel=on; got:\n"
      << s;
}

// Arming and then clearing must land back exactly where a reboot would.
TEST(ExpArms, ClearRestoresEveryDefault) {
  exparms::set(exparms::Arm::Row9Completion, false);
  EXPECT_FALSE(exparms::completionDrive());
  exparms::vidArm(5);
  EXPECT_TRUE(exparms::vidShortDu());
  exparms::clear();
  for (size_t i = 0; i < kArmCount; ++i) {
    const auto a = static_cast<exparms::Arm>(i);
    EXPECT_EQ(exparms::effective(a), exparms::buildDefault(a));
  }
  EXPECT_TRUE(exparms::completionDrive());
}

// No arm may be armed in any way that survives the process: the only storage this
// module touches is its own RAM statics, so a second "boot" (clear() == the reset
// path) is off. This test documents that constraint where the code is.
TEST(ExpArms, NothingPersistsAnArm) {
  exparms::set(exparms::Arm::Row9Completion, false);
  exparms::clear();
  EXPECT_TRUE(exparms::completionDrive());
  EXPECT_EQ(exparms::vidFramesRemaining(), 0u);
  exparms::set(exparms::Arm::FrontlightPanelPartial, false);
  EXPECT_FALSE(exparms::panelPartialRefresh());
  exparms::clear();
  EXPECT_TRUE(exparms::panelPartialRefresh()) << "'exp flpanel off' must not survive clear()";
}
