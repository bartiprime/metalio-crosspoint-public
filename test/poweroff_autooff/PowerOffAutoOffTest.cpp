// POWEROFF-AUTOOFF — the standby auto-off path must have the SD card mounted BEFORE it
// composes the power-off frame, and its journal line must say WHICH frame it painted.
//
// The defect this check exists to stop (PWR4, r101.21): the auto-off branch of
// metalioHardPowerOff(wakeAndPaintFirst=true) runs from INSIDE metalioStandbyLoop(), i.e.
// after metalioStandbySleep()'s park, which unmounts the card under
// CP_SD_PARK_ON_STANDBY (build default ON). The remount lives on the RESUME path, which
// this branch never reaches. metalioPaintPoweredOffFrame() dispatches on
// SETTINGS.powerOffScreen and POWER_OFF_CUSTOM reads /poweroff/* through Storage, so on that
// branch the operator's configured image could never be opened: paintCustomImageFrame()
// returned false and the frame silently degraded to the generic TEXT dialog. The same
// parked volume also swallowed the branch's own journal lines, so nothing on the card
// recorded that a substitution had happened.
//
// WHAT IS ASSERTED, READING THE PRODUCTION SOURCE (there is no seam under src/ and this
// batch may not add one, so a structural check is the only honest option — same technique
// as test/standby_resume_order and test/aud_gate):
//
//   L1  the wake-and-paint branch of metalioHardPowerOff() is located, and it calls
//       Storage.begin() BEFORE it repowers the panel or paints the frame;   <-- RED before
//       the fix (the branch had no mount at all);
//   L2  the branch's journal line names the frame that was painted
//       (metalioPowerOffFrameLabel()), so a degraded CUSTOM frame is visible from the
//       sleep journal alone;
//   L3  the mount is not silent: its own journal line carries an ok/FAILED verdict;
//   L4  NEGATIVE CONTROL in the other direction — the same predicate, run over a synthetic
//       pre-fix branch (repower, then paint, no mount), must FAIL, so a green L1 can never
//       mean "the predicate matches nothing";
//   L5  SCOPE: the non-wake branch (the confirmation dialog's shutdown, where the card is
//       still mounted) must NOT have gained a mount. The fix may only change the auto-off
//       path.

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

// Peel C++ comments so a landmark cannot be "found" in a comment that merely describes the
// code. src/main.cpp is full of such comments, and one of them used to say the card is
// "deliberately NOT remounted here" — the sentence this fix corrects.
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

// The auto-off branch: from `if (!wakeAndPaintFirst)` (the dialog branch) through the end of
// its `else` block, which is the last statement before the PMIC pulse trains.
struct AutoOffBranch {
  std::string wakePaint;   // the `else` arm: wake, mount, drive1, drive2, park
  std::string dialogArm;   // the `if (!wakeAndPaintFirst)` arm: park only, card still mounted
  bool found = false;
};

AutoOffBranch sliceAutoOffBranch(const std::string& code) {
  AutoOffBranch b;
  const size_t fn = code.find("static void metalioHardPowerOff");
  if (fn == std::string::npos) return b;
  const size_t gate = code.find("if (!wakeAndPaintFirst)", fn);
  if (gate == std::string::npos) return b;
  // The dialog arm's LAST statement is display.deepSleep(); the auto-off arm is the `else` that
  // follows it. Anchoring on that statement (not on the first "} else {" after the gate, which is
  // the PWR3 force-paint branch INSIDE the dialog arm) is what makes the slice unambiguous.
  const size_t park = code.find("display.deepSleep();", gate);
  if (park == std::string::npos) return b;
  const size_t elseAt = code.find("} else {", park);
  if (elseAt == std::string::npos) return b;
  // The branch ends with the re-park, immediately before the pulse-train loop.
  const size_t end = code.find("constexpr int kMaxPulseTrains", elseAt);
  if (end == std::string::npos) return b;
  b.dialogArm = code.substr(gate, elseAt - gate);
  b.wakePaint = code.substr(elseAt, end - elseAt);
  b.found = true;
  return b;
}

// L1's predicate, factored out so L4 can run the SAME rule over a synthetic branch. True when
// the mount call precedes both the panel repower and the frame paint.
bool mountPrecedesCompose(const std::string& branch) {
  const size_t mount = branch.find("Storage.begin()");
  const size_t repower = branch.find("metalio_vendor_panel_repower()");
  const size_t paint = branch.find("metalioPaintPoweredOffFrame(renderer)");
  if (mount == std::string::npos || repower == std::string::npos || paint == std::string::npos) return false;
  return mount < repower && repower < paint;
}

}  // namespace

TEST(PowerOffAutoOff, AutoOffBranchLocates) {
  const std::string code = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp"));
  ASSERT_FALSE(code.empty());
  const AutoOffBranch b = sliceAutoOffBranch(code);
  ASSERT_TRUE(b.found) << "the wake-and-paint branch of metalioHardPowerOff() was not located";
  EXPECT_NE(b.wakePaint.find("metalioWakePrimeDrive(renderer)"), std::string::npos);
  EXPECT_NE(b.wakePaint.find("metalioPaintPoweredOffFrame(renderer)"), std::string::npos);
}

// L1. RED before r101.21: the branch had no Storage.begin() at all, so a POWER_OFF_CUSTOM
// frame read /poweroff/* from an unmounted volume and fell through to TEXT.
TEST(PowerOffAutoOff, CardIsMountedBeforeTheFrameIsComposed) {
  const std::string code = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp"));
  ASSERT_FALSE(code.empty());
  const AutoOffBranch b = sliceAutoOffBranch(code);
  ASSERT_TRUE(b.found);
  ASSERT_TRUE(mountPrecedesCompose(b.wakePaint))
      << "the auto-off branch composes the power-off frame with the card parked; a "
         "POWER_OFF_CUSTOM frame then silently degrades to TEXT";
}

// L2. The journal line must name the frame that actually reached the glass.
TEST(PowerOffAutoOff, JournalNamesTheFrameThatWasPainted) {
  const std::string code = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp"));
  ASSERT_FALSE(code.empty());
  const AutoOffBranch b = sliceAutoOffBranch(code);
  ASSERT_TRUE(b.found);
  const size_t line = b.wakePaint.find("poweroff auto-off:");
  ASSERT_NE(line, std::string::npos) << "the auto-off sequence journal line is missing";
  const size_t label = b.wakePaint.find("metalioPowerOffFrameLabel()", line);
  ASSERT_NE(label, std::string::npos)
      << "the auto-off journal line does not say which frame was painted";
  EXPECT_NE(b.wakePaint.find("frame=%s", line), std::string::npos);
}

// L3. The mount itself is not silent -- a FAILED mount must be distinguishable from none.
TEST(PowerOffAutoOff, RemountIsJournaledWithItsVerdict) {
  const std::string code = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp"));
  ASSERT_FALSE(code.empty());
  const AutoOffBranch b = sliceAutoOffBranch(code);
  ASSERT_TRUE(b.found);
  const size_t j = b.wakePaint.find("SD remount %s");
  ASSERT_NE(j, std::string::npos) << "the auto-off mount has no journal line of its own";
  EXPECT_NE(b.wakePaint.find("sdVerdict", j), std::string::npos);
}

// L4. Negative control: the predicate must reject the pre-fix shape, or a green L1 is blind.
TEST(PowerOffAutoOff, NegativeControlOldOrderingFailsThePredicate) {
  // Verbatim shape of the branch before the fix: wake, repower, settle, drive1, drive2.
  const std::string preFix =
      "    } else {\n"
      "      epd_board_metalio_eink4_plus_bus_restore();\n"
      "      display.wake();\n"
      "      if (!metalio_vendor_panel_repower()) LOG_INF(\"PANEL\", \"x\");\n"
      "      delay(KWAKE_SETTLE_MS);\n"
      "      const unsigned long wakeDriveMs = metalioWakePrimeDrive(renderer);\n"
      "      const unsigned long frameDriveMs = metalioPaintPoweredOffFrame(renderer);\n";
  EXPECT_FALSE(mountPrecedesCompose(preFix));
}

// L5. SCOPE: only the auto-off branch may mount. The dialog's shutdown runs with the card
// mounted (the app is awake) and must be unchanged.
TEST(PowerOffAutoOff, DialogBranchDoesNotMount) {
  const std::string code = stripComments(readWholeFile(std::string(CP_REPO_ROOT) + "/src/main.cpp"));
  ASSERT_FALSE(code.empty());
  const AutoOffBranch b = sliceAutoOffBranch(code);
  ASSERT_TRUE(b.found);
  EXPECT_EQ(b.dialogArm.find("Storage.begin()"), std::string::npos)
      << "the non-auto-off power-off path gained a mount -- the fix must stay scoped to auto-off";
}
