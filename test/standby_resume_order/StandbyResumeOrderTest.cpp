// STANDBY-RESUME-ORDER — the touch wake must not be issued while the rail that
// feeds the touch controller is still down.
//
// The defect this check exists to stop (TCH3, r101.21 parked, unfixed): the
// standby resume path in metalioStandbySleep() calls
//
//     gpio.resumeAfterSleep()          // 0xA5 = 0x00 with a mode read-back
//
// BEFORE it restores the P0.5 CAM_SCR rail that the park cut. Under the CAM
// arm that is a register write to a controller which has lost its supply: the
// I2C transaction NAKs, the wake never lands, touchIrqReady stays armed from
// the previous session, and the UI silently takes its non-touch branch for the
// whole session (PARKED_R10121.md §P1, VENDOR_PARITY_PLAN.md §2.3(e)).
//
// What is asserted, reading the PRODUCTION SOURCE (there is no seam under src/
// and this batch may not add one, so a structural check is the only honest
// option — same technique as test/aud_gate):
//
//   L1  the resume region of metalioStandbySleep() is located, and both landmarks
//       (the P0.5 restore and the touch wake) are present in it;
//   L2  the P0.5 restore comes BEFORE gpio.resumeAfterSleep();   <-- RED today
//   L3  the order around the card is asserted too, and it is the MIRROR OF THE PARK:
//       P1.4 -> P0.5 -> SD remount. (r108 ORD2. Until r108 this line asserted the
//       OPPOSITE -- "SD remount first" -- which is what r107 built; the park unmounts
//       the card before it cuts the rails, so the resume must bring the rails back
//       before it re-initialises the SDMMC bus.)
//
// L2 is deliberately registered RED in test/EXPECTED_RED.txt: the bug is real
// and the fix (VENDOR_PARITY_PLAN.md §3 step 3, a one-line move) is parked with
// TCH3. When the move lands, this test goes green and the register line must be
// deleted in the same commit — the runner fails the gate if it stays.
//
// The last test is a negative control in the other direction: it runs the same
// predicate over a synthetic "already fixed" ordering and requires it to pass,
// so a green L2 can never mean "the predicate matches nothing".

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

// Peel C++ comments so a landmark cannot be "found" in a comment that merely
// describes the code. src/main.cpp is full of such comments, including several
// that name this exact call pair.
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

// Body text (without the outer braces) of the block whose header contains
// `headerNeedle`.
std::string blockBody(const std::string& src, const std::string& headerNeedle, bool* found = nullptr) {
  const size_t at = src.find(headerNeedle);
  if (found) *found = (at != std::string::npos);
  if (at == std::string::npos) return {};
  const size_t open = src.find('{', at + headerNeedle.size());
  const size_t close = matchBrace(src, open);
  if (open == std::string::npos || close == std::string::npos) return {};
  return src.substr(open + 1, close - open - 1);
}

// ---------------------------------------------------------------------------
// The ordering predicate, as a pure function of one source body. Kept separate
// from the file read so the negative control can run it on a literal.
// ---------------------------------------------------------------------------

struct ResumeOrder {
  size_t parkLoop = std::string::npos;    // metalioStandbyLoop() -- the pivot
  size_t sdRemount = std::string::npos;   // Storage.begin() on the resume path
  size_t touchWake = std::string::npos;   // gpio.resumeAfterSleep()
  size_t paRestore = std::string::npos;   // pa_rail_set(true)
  size_t camRestore = std::string::npos;  // cam_rail_set(true)

  bool landmarksFound() const { return sdRemount != std::string::npos && touchWake != std::string::npos &&
                                       paRestore != std::string::npos && camRestore != std::string::npos; }

  // The rule TCH3 is about.
  bool camRestorePrecedesTouchWake() const { return camRestore < touchWake; }
  // The rules the fix must not break: rails first, then the card (the mirror of the
  // park's unmount -> rails), and P1.4 before P0.5 inside that.
  bool railsRestoreBeforeTheRemount() const { return paRestore < sdRemount && camRestore < sdRemount; }
  bool paRestorePrecedesCamRestore() const { return paRestore < camRestore; }
};

// Search for a landmark, but never one that sits before `from` (so a boot-path
// call elsewhere in the same file cannot be mistaken for the resume one).
size_t findAfter(const std::string& hay, const std::string& needle, size_t from) {
  if (from == std::string::npos) return std::string::npos;
  const size_t at = hay.find(needle, from);
  return at;
}

ResumeOrder analyse(const std::string& body) {
  ResumeOrder o;
  o.parkLoop = body.find("metalioStandbyLoop()");
  if (o.parkLoop == std::string::npos) return o;
  // Everything the resume does happens after the standby loop returns.
  o.sdRemount = findAfter(body, "Storage.begin()", o.parkLoop);
  o.touchWake = findAfter(body, "gpio.resumeAfterSleep()", o.parkLoop);
  o.paRestore = findAfter(body, "epd_board_metalio_eink4_plus_pa_rail_set(true)", o.parkLoop);
  o.camRestore = findAfter(body, "epd_board_metalio_eink4_plus_cam_rail_set(true)", o.parkLoop);
  return o;
}

const std::string kMainPath = std::string(CP_REPO_ROOT) + "/src/main.cpp";

struct ResumeSource {
  std::string raw;
  std::string code;
  std::string body;
  bool found = false;
  ResumeOrder order;
};

const ResumeSource& resumeSource() {
  static const ResumeSource s = [] {
    ResumeSource r;
    r.raw = readWholeFile(kMainPath);
    r.code = stripComments(r.raw);
    r.body = blockBody(r.code, "static void metalioStandbySleep(", &r.found);
    if (r.found) r.order = analyse(r.body);
    return r;
  }();
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// L1 — the source is readable and both landmarks are inside the resume path.
// ---------------------------------------------------------------------------

TEST(StandbyResumeOrder, ResumePathIsLocatable) {
  const auto& s = resumeSource();
  ASSERT_FALSE(s.raw.empty()) << "cannot read " << kMainPath;
  ASSERT_TRUE(s.found) << "metalioStandbySleep() not found in src/main.cpp";
  ASSERT_FALSE(s.body.empty()) << "metalioStandbySleep() has an empty or unbalanced body";
  ASSERT_NE(s.order.parkLoop, std::string::npos)
      << "the resume path no longer calls metalioStandbyLoop(): is this still the standby entry point?";
}

TEST(StandbyResumeOrder, BothRailAndTouchLandmarksAreOnTheResumePath) {
  const auto& s = resumeSource();
  ASSERT_TRUE(s.found);
  EXPECT_NE(s.order.touchWake, std::string::npos)
      << "no gpio.resumeAfterSleep() after the standby loop: the touch wake is gone from the resume path";
  EXPECT_NE(s.order.camRestore, std::string::npos)
      << "no cam_rail_set(true) after the standby loop: either the P0.5 arm was deleted or it moved behind "
         "a wrapper this check cannot see -- fix the check, do not weaken it";
}

// ---------------------------------------------------------------------------
// L2 — THE DEFECT. Registered RED (test/EXPECTED_RED.txt, TCH3).
// ---------------------------------------------------------------------------

TEST(StandbyResumeOrder, TouchWakeIsNotIssuedBeforeTheCamRailIsRestored) {
  const auto& s = resumeSource();
  ASSERT_TRUE(s.found);
  ASSERT_NE(s.order.touchWake, std::string::npos);
  ASSERT_NE(s.order.camRestore, std::string::npos);
  EXPECT_TRUE(s.order.camRestorePrecedesTouchWake())
      << "P0.5 CAM_SCR restore is at offset " << s.order.camRestore << " but gpio.resumeAfterSleep() is at "
      << s.order.touchWake << ": the FT6336U wake (0xA5->0x00) is written while the rail that feeds the "
         "controller is still DOWN, so the write NAKs and touch never comes back. Defect TCH3; fix is "
         "VENDOR_PARITY_PLAN.md §3 step 3 -- move the touch wake after the P0.5 restore.";
}

// ---------------------------------------------------------------------------
// L3 — the ordering the fix must preserve (§2.3(f)).
// ---------------------------------------------------------------------------

// SD last: the card's bus is re-initialised only after the rails that feed it are back
// (r108 ORD2 -- the exact mirror of the park, which unmounts the card before it cuts).
TEST(StandbyResumeOrder, RailsComeBackBeforeTheCardRemounts) {
  const auto& s = resumeSource();
  ASSERT_TRUE(s.found);
  ASSERT_NE(s.order.sdRemount, std::string::npos) << "no Storage.begin() on the resume path: CP_SD_PARK_ON_STANDBY "
                                                     "must still remount the card on wake";
  EXPECT_TRUE(s.order.railsRestoreBeforeTheRemount())
      << "the SD remount is at offset " << s.order.sdRemount << ", P1.4 restore at " << s.order.paRestore
      << " and P0.5 restore at " << s.order.camRestore
      << ": the card is being re-initialised on a bus whose rails are still down, which is the "
         "mirror of the r105 defect the park half was fixed for (r108 ORD2).";
}

// P1.4 before P0.5: raising the second load on a rail that is not up stacks two
// inrush events inside one PWR_GOOD window (vendor rule, §2.3(f)).
TEST(StandbyResumeOrder, PaRailRestoreStaysBeforeCamRestore) {
  const auto& s = resumeSource();
  ASSERT_TRUE(s.found);
  ASSERT_NE(s.order.paRestore, std::string::npos);
  ASSERT_NE(s.order.camRestore, std::string::npos);
  EXPECT_TRUE(s.order.paRestorePrecedesCamRestore())
      << "P0.5 is restored before P1.4: the vendor order is P14 -> 80 ms settle -> P5";
}

// ---------------------------------------------------------------------------
// NEGATIVE CONTROL — the predicate must PASS on an already-correct ordering.
// If this ever fails, the check above has stopped detecting anything and its
// red says nothing about the firmware.
// ---------------------------------------------------------------------------

TEST(StandbyResumeOrder, FixedOrderSatisfiesTheSamePredicate) {
  // The shape the fix produces (r108 ORD2): park loop returns, P1.4 comes back
  // with its settle, P0.5 comes back, the SD is remounted on a live bus, and only
  // THEN is the touch woken.
  static const std::string kFixed = R"__(
    metalioStandbyLoop();
    if (paActive) { epd_board_metalio_eink4_plus_pa_rail_set(true); delay(80); }
    if (camActive) { epd_board_metalio_eink4_plus_cam_rail_set(true); }
    if (sdParked) { Storage.begin(); }
    const bool touchOk = gpio.resumeAfterSleep();
  )__";
  const ResumeOrder o = analyse(kFixed);
  EXPECT_TRUE(o.landmarksFound()) << "the predicate cannot even find its landmarks in the fixed ordering";
  EXPECT_TRUE(o.camRestorePrecedesTouchWake()) << "the fixed ordering was rejected by the predicate";
  EXPECT_TRUE(o.paRestorePrecedesCamRestore());
  EXPECT_TRUE(o.railsRestoreBeforeTheRemount());
}
