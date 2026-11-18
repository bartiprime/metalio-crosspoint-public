// r163 E6 / 3.5 -- the WIRING, as text over the production sources.
//
// There is no seam under src/ for main.cpp (a single sketch translation unit) and
// this batch may not add one, so the structural checks below are the honest option
// -- the same technique as test/standby_resume_order and test/aud_gate.
//
// The claim this file has to make is narrow and specific:
//
//   W1  the auto-sleep decision asks powerneed::blocksAutoStandby() and still calls
//       enterDeepSleep(true) in the same branch -- one gate, not a second sleep path;
//   W2  metalioStandbySleep() -- the park/resume sequence, the rail order, the
//       settle constants -- contains NO power-need code at all. That is the proof
//       that a need can only delay the call, never change what the call does;
//   W3  a hard release refreshes the inactivity clock (takeActivityRefresh) and the
//       tick runs BEFORE the timeout is compared;
//   W4  the charge policy is a need held from the tick, not a branch in the park;
//   W5  the USB drive-mode session takes and releases UsbVirtualDisk;
//   W6  the setting is persisted, surfaced, and carryable through a backup restore.

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
      ++i;  // CRLF sources compare as LF, so string literals below can be written once
    } else {
      out.push_back(src[i]);
      ++i;
    }
  }
  return out;
}

size_t findOrFail(const std::string& hay, const std::string& needle) {
  const size_t p = hay.find(needle);
  EXPECT_NE(p, std::string::npos) << "not found: " << needle;
  return p;
}

// The body of a function, from its opening brace to the matching one.
std::string blockBody(const std::string& src, const std::string& signature, bool* found) {
  *found = false;
  const size_t sig = src.find(signature);
  if (sig == std::string::npos) return {};
  const size_t open = src.find('{', sig);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    if (src[i] == '}') {
      --depth;
      if (depth == 0) {
        *found = true;
        return src.substr(open, i - open + 1);
      }
    }
  }
  return {};
}

class PowerNeedWiring : public ::testing::Test {
 protected:
  void SetUp() override {
    main_ = readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp");
    ASSERT_FALSE(main_.empty()) << "src/main.cpp not readable from " << CP_REPO_ROOT;
    mainNoComments_ = stripComments(main_);
  }
  std::string main_;
  std::string mainNoComments_;
};

// W1 -- one gate, in front of the existing call.
TEST_F(PowerNeedWiring, AutoSleepIsGatedByAHardNeedAndStillCallsTheSameSleep) {
  const std::string& s = mainNoComments_;
  const size_t gate = findOrFail(s, "powerneed::blocksAutoStandby()");
  const size_t sleep = findOrFail(s, "enterDeepSleep(true);");
  ASSERT_LT(gate, sleep) << "the need must be asked before the sleep is entered";
  // The gate and the call are in the same if/else chain: between them there must be
  // no other enterDeepSleep(), which would mean a second sleep path.
  const size_t between = s.find("enterDeepSleep(", gate);
  EXPECT_EQ(between, sleep);
}

// W2 -- the park sequence is untouched, by construction: no need code lives in it.
TEST_F(PowerNeedWiring, TheParkSequenceContainsNoPowerNeedCode) {
  bool found = false;
  const std::string body = blockBody(mainNoComments_, "static void metalioStandbySleep(", &found);
  ASSERT_TRUE(found) << "metalioStandbySleep() not found in src/main.cpp";
  ASSERT_FALSE(body.empty());
  EXPECT_EQ(body.find("powerneed"), std::string::npos)
      << "a power need must never be read, acquired or released inside the park/resume sequence";
  EXPECT_EQ(body.find("chargepolicy"), std::string::npos);
}

// W3 -- the tick runs before the comparison, and a hard release moves the clock.
TEST_F(PowerNeedWiring, TickPrecedesTheTimeoutAndHardReleasesRefreshTheClock) {
  const std::string& s = mainNoComments_;
  const size_t tick = findOrFail(s, "powerNeedTick();");
  const size_t refresh = findOrFail(s, "powerneed::sys().takeActivityRefresh()");
  const size_t timeout = findOrFail(s, "const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();");
  EXPECT_LT(tick, refresh);
  EXPECT_LT(refresh, timeout) << "the refresh must land BEFORE the clock is compared";
  EXPECT_LT(tick, timeout);
  // The resume still refreshes the idle clock, and the need ages are rebased at the same
  // point -- a park spans wall clock, so it must never be read as a leak.
  const size_t rebase = findOrFail(s, "powerneed::sys().rebase(millis());");
  const size_t resumeRefresh = s.rfind("lastActivityTime = millis();", rebase);
  ASSERT_NE(resumeRefresh, std::string::npos);
  EXPECT_LT(resumeRefresh, rebase);
  EXPECT_LT(rebase - resumeRefresh, 400u) << "the rebase belongs to the resume block, not some earlier line";
}

// W4 -- 3.5 is a need held from the tick, fed by the persisted setting.
TEST_F(PowerNeedWiring, ChargePolicyIsAHardNeedHeldFromTheTick) {
  const std::string& s = mainNoComments_;
  EXPECT_NE(s.find("chargepolicy::holdWhileCharging(SETTINGS.chargeIdlePolicy, powerManager.isCharging())"),
            std::string::npos);
  EXPECT_NE(s.find("powerneed::Need::ChargerHost"), std::string::npos);
  EXPECT_NE(s.find("powerneed::set(powerneed::Need::ChargerHost"), std::string::npos);
}

// W5 -- the USB drive-mode session is the UsbVirtualDisk holder, and it lets go.
TEST(PowerNeedWiringUsbMsc, SessionTakesAndReleasesTheHardNeed) {
  const std::string src =
      stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/network/UsbMscActivity.cpp"));
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("powerNeed_.acquire(millis());"), std::string::npos);
  const size_t acquire = src.find("powerNeed_.acquire(millis());");
  const size_t release = src.find("powerNeed_.release(millis());");
  EXPECT_NE(release, std::string::npos) << "a hold with no release in the same file is a leak by definition";
  EXPECT_LT(acquire, release);

  const std::string header =
      readWholeFile(std::string(CP_REPO_ROOT) + "/src/activities/network/UsbMscActivity.h");
  EXPECT_NE(header.find("powerneed::ScopedNeed powerNeed_{powerneed::Need::UsbVirtualDisk}"), std::string::npos);
}

// W6 -- the setting: default, row, and the backup rule that carries it across a restore.
TEST(PowerNeedWiringSettings, ChargeIdlePolicyIsPersistedSurfacedAndBackedUp) {
  const std::string settings = readWholeFile(std::string(CP_REPO_ROOT) + "/src/CrossPointSettings.h");
  ASSERT_FALSE(settings.empty());
  EXPECT_NE(settings.find("uint8_t chargeIdlePolicy = 0;"), std::string::npos)
      << "the default must be 0 (follow the timeout) -- changing a user's sleep policy on a cable is the surprise";

  const std::string list = readWholeFile(std::string(CP_REPO_ROOT) + "/src/SettingsList.cpp");
  ASSERT_FALSE(list.empty());
  EXPECT_NE(list.find("\"chargeIdlePolicy\""), std::string::npos) << "no UI row = no way to turn it on";

  const std::string backup = readWholeFile(std::string(CP_REPO_ROOT) + "/lib/Backup/BackupProfile.cpp");
  ASSERT_FALSE(backup.empty());
  EXPECT_NE(backup.find("{\"chargeIdlePolicy\", 0, 1}"), std::string::npos)
      << "a restore writes the sanitized object as the whole settings file: an unlisted key is dropped";

  const std::string english = readWholeFile(std::string(CP_REPO_ROOT) + "/lib/I18n/translations/english.yaml");
  ASSERT_FALSE(english.empty());
  EXPECT_NE(english.find("STR_CHARGE_KEEP_AWAKE"), std::string::npos);
}

// W7 -- the leak is visible on the debug surface, not only in the log.
TEST(PowerNeedWiringDebug, TheShellExposesTheNeedTable) {
  const std::string shell = readWholeFile(std::string(CP_REPO_ROOT) + "/src/debug/UsbShell.cpp");
  ASSERT_FALSE(shell.empty());
  EXPECT_NE(shell.find("int cmdNeeds("), std::string::npos);
  EXPECT_NE(shell.find("{\"needs\","), std::string::npos) << "the command must be in the table the shell serves";
  EXPECT_NE(shell.find("powerneed::sys().report(now)"), std::string::npos);
  EXPECT_NE(shell.find("powerneed::holdName"), std::string::npos) << "hard/soft must be named, not implied";
}

}  // namespace
