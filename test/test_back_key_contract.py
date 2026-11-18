"""The r163 Back-key contract: TAP = one level back, HOLD = Home, firmware-wide.

Run with python3 test/test_back_key_contract.py (requires a host C++ compiler).

Two production bodies are compiled verbatim against stubs, the same way
test_remote_button_input.py loads MappedInputManager::update:

  A. ActivityManager::handleGlobalBackKey() -- the one place that decides the tap/hold split for every
     screen. It is where the reported defect lived: the hold branch runs BEFORE the ownsBackKey()
     check, so an untimeable press (a press start that predates the screen, i.e. HalGPIO's
     buttonPressStart stamped by an earlier contact) navigated Home out of Settings > System, whose
     class answers no Back itself.

  B. MappedInputManager::wasLongPressed() -- the threshold event itself, now gated on the moment the
     press was OBSERVED (the r162 allowSleepAt shape).
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def host_compiler():
    """First C++ compiler that answers --version (CXX wins, then the usual names)."""
    for candidate in (os.environ.get("CXX"), "clang++", "g++", "c++"):
        if not candidate:
            continue
        found = shutil.which(candidate)
        if found and subprocess.run([found, "--version"], capture_output=True).returncode == 0:
            return found
    raise SystemExit("test_back_key_contract: no C++ compiler found (set CXX)")
manager_source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
mapped_source = (ROOT / "src/MappedInputManager.cpp").read_text()

# --- A: the production tap/hold decision -------------------------------------------------------
handle_back = "bool ActivityManager::handleGlobalBackKey()" + manager_source.split(
    "bool ActivityManager::handleGlobalBackKey()", 1
)[1].split("void ActivityManager::loop()", 1)[0]

harness_a = (
    r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// AUDIO1: the tap path emits a UI click. Nothing in this contract is about the sound, so it is a
// no-op sink.
//
// r165.5: the sink is no longer a hand-copied `namespace uisounds { enum class Event ... }`. Both
// src/audio/UiSoundPolicy.h and src/audio/UiSounds.h are host-clean (cstdint only -- no Arduino, no
// FreeRTOS, no btaudio), so the REAL declarations are included and only the definition is stubbed.
// That is the guard that was missing: r165.4 changed the signature to onEvent(Event, Tag) and this
// harness kept compiling against its private copy of the old one -- except it did not, it stopped
// compiling at all and nobody noticed for a whole round because the harness was not in ctest. With
// the real declaration in scope, a future signature change makes the definition below stop matching
// it (redeclaration mismatch, or an ambiguous overload at the call site) and the build fails HERE,
// naming this file -- which is the loud failure this contract needs.
#include "audio/UiSounds.h"

namespace uisounds {
void onEvent(Event, Tag) {}  // must match the declaration in audio/UiSounds.h exactly
}  // namespace uisounds

unsigned long nowMs = 0;
unsigned long millis() { return nowMs; }
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)

struct Input {
  enum class Button { Back, Confirm };
  bool pressed = false, released = false, held = false;
  unsigned long heldTime = 0;
  mutable bool longPressSpent = false;
  bool wasPressed(Button) const { return pressed; }
  bool wasReleased(Button) const { return released; }
  bool isPressed(Button) const { return held; }
  unsigned long getHeldTime() const { return heldTime; }
  // Mirrors MappedInputManager::wasLongPressed's contract, including the r163 gate, so the manager's
  // own logic (visit latch + notBefore plumbing) is what is under test here. The real gate is
  // exercised by harness B below.
  bool wasLongPressed(Button, unsigned long thresholdMs, unsigned long notBeforeMs = 0) const {
    if (!held || longPressSpent || heldTime < thresholdMs) return false;
    if (notBeforeMs != 0 && millis() - heldTime < notBeforeMs) return false;
    longPressSpent = true;
    return true;
  }
  bool wasBackControlHold(unsigned long = 0, unsigned long = 0) const { return false; }
} mappedInput;
using MappedInputManager = Input;

struct Activity {
  std::string name = "Settings";
  bool home = false, owns = false, claimsHold = false;
  bool isHomeActivity() const { return home; }
  bool ownsBackKey() const { return owns; }
  bool handleLongPressBack() { return claimsHold; }
} screen;

struct ActivityManager {
  enum class PendingAction { None } pendingAction = PendingAction::None;
  Activity* currentActivity = &screen;
  std::vector<int> stackActivities;
  bool backPressLatched = false;
  uint32_t backPressVisit = 0, screenVisit = 0;
  unsigned long backPressSeenMs = 0, screenVisitStartMs = 0;
  static constexpr unsigned long kLongPressBackHomeMs = 1200;
  int pops = 0, homes = 0;
  void popActivity() {
    ++pops;
    if (stackActivities.empty())
      ++homes;  // the pop path lands on Home at the bottom of the stack
    else
      stackActivities.pop_back();
  }
  void goHome() { ++homes; }
  bool handleGlobalBackKey();
};
"""
    + handle_back
    + r"""
// ---- helpers ---------------------------------------------------------------------------------
ActivityManager mgr;

// Every contract below ends in one CASEOK line, so a run that stops at case 5 says exactly which
// contracts held. The count is asserted at the end: a case deleted without updating kCases fails.
int cases = 0;
#define CASEOK(...) do { ++cases; std::printf("  [%d] ", cases); std::printf(__VA_ARGS__); } while (0)
constexpr int kCases = 8;

void reset() {
  mgr = ActivityManager{};
  screen = Activity{};
  mappedInput = Input{};
  nowMs = 1000;
}

// A press-down the current screen observed.
void pressDown() {
  mappedInput.pressed = true;
  mappedInput.released = false;
  mappedInput.held = true;
  mgr.handleGlobalBackKey();  // the pass that stamps the latch
  mappedInput.pressed = false;
}

// The release of that same press.
void release(const unsigned long heldMs = 90) {
  mappedInput.held = false;
  mappedInput.heldTime = heldMs;
  mappedInput.released = true;
  mgr.handleGlobalBackKey();
  mappedInput.released = false;
}

void pass(const unsigned long advanceMs) { nowMs += advanceMs; }

int main() {
  // 1. A single tap pops exactly one level (Settings > System -> settings root), never Home.
  reset();
  mgr.stackActivities.push_back(1);
  pressDown();
  mappedInput.heldTime = 90;
  pass(90);
  release();
  assert(mgr.pops == 1);
  assert(mgr.homes == 0);
  CASEOK("tap pops exactly one level: pops=%d homes=%d\n", mgr.pops, mgr.homes);

  // 2. A press whose start this screen never observed is NOT a hold, even though the pipeline's
  //    stamp says it has been down for seconds. Before r163 this was the Settings > System defect:
  //    the hold branch navigated Home before ownsBackKey() could answer.
  reset();
  mgr.stackActivities.push_back(1);
  mappedInput.held = true;
  mappedInput.heldTime = 5000;  // implied press start = year zero, latch was never stamped
  pass(5000);
  assert(!mgr.handleGlobalBackKey());
  assert(mgr.homes == 0 && mgr.pops == 0);
  CASEOK("unobserved press (stale pipeline stamp) is never a hold: homes=0 pops=0\n");

  // 2b. Same, but the stale latch belongs to the PREVIOUS screen visit.
  reset();
  mgr.stackActivities.push_back(1);
  pressDown();             // observed on visit 0
  mgr.screenVisit = 1;     // a transition happened with the finger still down
  mgr.screenVisitStartMs = nowMs;
  mappedInput.heldTime = 5000;
  pass(5000);
  assert(!mgr.handleGlobalBackKey());
  assert(mgr.homes == 0);
  CASEOK("carried-over press from a previous visit is never a hold: homes=0 pops=0\n");

  // 3. A genuine hold still goes Home, at the same 1200 ms, from a screen that owns Back.
  reset();
  screen.owns = true;
  mgr.stackActivities.push_back(1);
  pressDown();  // observed at nowMs = 1000
  mappedInput.heldTime = 1199;
  pass(1199);
  assert(!mgr.handleGlobalBackKey());  // one ms short: still a press, not a hold
  assert(mgr.homes == 0 && mgr.pops == 0);
  mappedInput.heldTime = 1200;
  pass(1);
  assert(mgr.handleGlobalBackKey());
  assert(mgr.homes == 1);
  CASEOK("genuine >=1200 ms hold goes Home from an ownsBackKey screen: homes=%d\n", mgr.homes);

  // 4. Double tap = two pops.
  reset();
  mgr.stackActivities.push_back(1);
  mgr.stackActivities.push_back(2);
  for (int i = 0; i < 2; ++i) {
    pressDown();
    mappedInput.heldTime = 80;
    pass(80);
    release();
    pass(120);
  }
  assert(mgr.pops == 2);
  assert(mgr.homes == 0);
  CASEOK("double tap pops twice: pops=%d homes=%d\n", mgr.pops, mgr.homes);

  // 5. A screen that owns Back still answers its own release (the manager stays out of it).
  reset();
  screen.owns = true;
  mgr.stackActivities.push_back(1);
  pressDown();
  mappedInput.heldTime = 90;
  pass(90);
  release();
  assert(mgr.pops == 0 && mgr.homes == 0);
  CASEOK("ownsBackKey screen answers its own release: pops=0 homes=0\n");

  // 6. Home is the bottom of the stack: tap and hold are both inert there.
  reset();
  screen.home = true;
  pressDown();
  mappedInput.heldTime = 5000;
  pass(5000);
  assert(!mgr.handleGlobalBackKey());
  assert(mgr.homes == 0 && mgr.pops == 0);
  release();
  assert(mgr.homes == 0);
  CASEOK("Home is inert on tap and hold: homes=0 pops=0\n");

  // 7. An activity that claims the hold (handleLongPressBack) keeps it: no Home from the manager.
  reset();
  screen.claimsHold = true;
  pressDown();
  mappedInput.heldTime = 2000;
  pass(2000);
  assert(!mgr.handleGlobalBackKey());
  assert(mgr.homes == 0);
  CASEOK("handleLongPressBack() still owns the hold: homes=0\n");
  assert(cases == kCases);
  std::printf("back_key (manager): %d/%d contracts OK\n", cases, kCases);
}
"""
)

# --- B: the production threshold event ----------------------------------------------------------
was_long_pressed = "bool MappedInputManager::wasLongPressed(" + mapped_source.split(
    "bool MappedInputManager::wasLongPressed(", 1
)[1].split("\n}\n", 1)[0] + "\n}\n"

harness_b = (
    r"""
#include <cassert>
#include <cstdint>
#include <cstdio>

unsigned long nowMs = 0;
unsigned long millis() { return nowMs; }

struct CrossPointSettings {
  int softKeyHaptic = 0;
} SETTINGS;
namespace haptic_feedback {
inline void deferPageTurnAction(bool = false) {}
}  // namespace haptic_feedback

struct HalGPIO {
  static constexpr uint8_t BTN_LEFT = 2, BTN_RIGHT = 3;
  bool pressed = false, pageKey = false;
  bool isPressed(uint8_t) const { return pressed; }
  bool isCapacitivePagePressed(uint8_t) const { return false; }
};
class MappedInputManager {
 public:
  enum class Button { Back, Confirm };
  HalGPIO gpio;
  mutable uint16_t longPressFiredButtons = 0;
  bool held = false;
  unsigned long heldMs = 0;
  bool isPressed(Button) const { return held; }
  unsigned long getHeldTime() const { return heldMs; }
  bool mapButton(Button, bool (HalGPIO::*fn)(uint8_t) const) const { return (gpio.*fn)(0); }
  bool coverGlassKeysAreButtonBits() const { return false; }
  void suppressNextRelease(Button) const {}
  bool wasLongPressed(Button button, unsigned long thresholdMs, unsigned long notBeforeMs = 0) const;
};
"""
    + was_long_pressed
    + r"""
int main() {
  MappedInputManager input;
  using Button = MappedInputManager::Button;
  constexpr unsigned long kHold = 1200;
  int cases = 0;
#define CASEOK(...) do { ++cases; std::printf("  [%d] ", cases); std::printf(__VA_ARGS__); } while (0)
  constexpr int kCases = 4;

  // A genuine hold: the press edge was observed at nowMs = 1000 (so the implied press start matches)
  // and 1200 ms of real contact have passed. It fires, and only once.
  nowMs = 1000;
  input.held = true;
  input.heldMs = 1;
  assert(!input.wasLongPressed(Button::Back, kHold, 1000));  // press edge: not yet a hold
  nowMs = 2200;
  input.heldMs = 1200;                                       // implied start = 1000
  assert(input.wasLongPressed(Button::Back, kHold, 1000));
  assert(!input.wasLongPressed(Button::Back, kHold, 1000));  // one shot per contact
  CASEOK("genuine hold fires (observed at its own press start): OK\n");

  // A press whose start cannot be timed -- the pipeline's implied start (nowMs - heldMs) predates the
  // moment this screen saw the press go down -- is never a hold, and is not consumed either, so the
  // press can still be answered as a tap.
  nowMs = 9000;
  input.held = true;
  input.heldMs = 9000;  // implied start = 0
  input.longPressFiredButtons = 0;
  assert(!input.wasLongPressed(Button::Back, kHold, 5000));
  assert(input.longPressFiredButtons == 0);
  CASEOK("untimeable press is never a hold: OK\n");

  // Ungated callers (the default argument) keep the pre-r163 behaviour exactly.
  assert(input.wasLongPressed(Button::Back, kHold));
  CASEOK("notBeforeMs is opt-in: OK\n");

  // A short press is never a hold, gated or not.
  input.longPressFiredButtons = 0;
  input.heldMs = 90;
  nowMs = 1090;
  assert(!input.wasLongPressed(Button::Back, kHold, 1000));
  CASEOK("short press is never a hold, gated or not: OK\n");
  assert(cases == kCases);
  std::printf("back_key (hold event): %d/%d contracts OK\n", cases, kCases);
}
"""
)

# --- C: the reader's one-level tap (where a book goes on Back) ----------------------------------
# The reader is entered with replaceActivity(), so there is no stack entry to pop and the destination
# has to be recorded when the book is opened. Both production bodies are compiled verbatim.
def _function_body(source, signature):
    return signature + source.split(signature, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


note_origin = _function_body(manager_source, "void ActivityManager::noteReaderOrigin(")
leave_origin = _function_body(manager_source, "void ActivityManager::leaveReaderToOrigin(")
# D2: the session term. Absent on a pre-r165.3 tree, where nothing but noteReaderOrigin()'s own
# isReaderActivity() test decided the target -- which is exactly the defect this case measures, so
# the harness still builds and still judges that tree (it fails case 4).
if "void ActivityManager::noteReaderSessionReplaced(" in manager_source:
    session_fn = _function_body(manager_source, "void ActivityManager::noteReaderSessionReplaced(")
else:
    session_fn = "void ActivityManager::noteReaderSessionReplaced(bool, const char*) {}\n"

harness_c = (
    r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

// The manager as the REAL chain uses it: every navigation is a replaceActivity(), so the harness
// reports each incoming screen to noteReaderSessionReplaced() and only then makes it the screen in
// front -- the same order as ActivityManager::replaceActivity().
struct ActivityManager {
  enum class ReaderReturnTarget : uint8_t { FileBrowser, Library, Home };
  ReaderReturnTarget readerReturnTarget = ReaderReturnTarget::FileBrowser;
  bool readerSessionOpen = false;
  bool readerUnder = false;  // a reader is in front or on the stack (the pre-r165.3 term)
  int toLibrary = 0, toHome = 0, toBrowser = 0;
  std::string browserPath;
  bool isReaderActivity() const { return readerUnder; }
  void goToLibrary() { ++toLibrary; }
  void goHome() { ++toHome; }
  void goToFileBrowser(const std::string& path) {
    ++toBrowser;
    browserPath = path;
  }
  // ActivityManager::replaceActivity(): the incoming screen, then it is the one in front.
  void replace(const bool incomingIsReader, const char* incomingName) {
    noteReaderSessionReplaced(incomingIsReader, incomingName);
    readerUnder = incomingIsReader;
  }
  void noteReaderOrigin(const std::string& from);
  void noteReaderSessionReplaced(bool incomingIsReader, const char* incomingName);
  void leaveReaderToOrigin(const std::string& path);
};
"""
    + note_origin
    + session_fn
    + leave_origin
    + r"""
void reset(ActivityManager& m) { m = ActivityManager{}; }

int main() {
  ActivityManager m;
  int cases = 0;
#define CASEOK(...) do { ++cases; std::printf("  [%d] ", cases); std::printf(__VA_ARGS__); } while (0)
  constexpr int kCases = 9;

  // 1. A book opened from the Library goes back to the Library, one level -- not to the file browser
  //    at the book's folder, which is two levels out (what r163 did for every entry).
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/a.epub");
  assert(m.toLibrary == 1 && m.toBrowser == 0 && m.toHome == 0);
  CASEOK("reader from Library -> Library: library=%d browser=%d\n", m.toLibrary, m.toBrowser);

  // 2. A book opened from Home's recents goes back to Home.
  reset(m);
  m.replace(false, "Home");
  m.noteReaderOrigin("Home");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/a.epub");
  assert(m.toHome == 1 && m.toLibrary == 0 && m.toBrowser == 0);
  CASEOK("reader from Home -> Home: home=%d library=%d\n", m.toHome, m.toLibrary);

  // 3. A book opened from the file browser goes back to the browser, at the book's own folder.
  reset(m);
  m.replace(false, "FileBrowser");
  m.noteReaderOrigin("FileBrowser");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/a.epub");
  assert(m.toBrowser == 1 && m.browserPath == "/books/a.epub" && m.toLibrary == 0);
  CASEOK("reader from FileBrowser -> browser at the book's folder: browser=%d\n", m.toBrowser);

  // 4. THE OPERATOR'S COMPLAINT, as the real chain runs it:
  //      Library -> book -> KOReader sync -> book -> Back
  //    KOReaderSyncActivity REPLACES the reader (EpubReaderActivity hands it the whole screen), so
  //    while syncing there is no reader in front and none on the stack -- which is precisely why the
  //    old `!isReaderActivity()` test wiped the Library target. It is still the SAME session.
  //    PRE-FIX VERDICT: leaveReaderToOrigin() went to the file browser (library=0 browser=1) and the
  //    assertion below failed. POST-FIX: it lands on the Library.
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");      // Library -> book
  m.replace(true, "EpubReader");      // the book is in front
  m.replace(false, "KOReaderSync");   // the sync screen replaces the reader...
  m.noteReaderOrigin("KOReaderSync"); // ...and restarts into a book
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/b.epub");
  CASEOK("   Library -> book -> sync -> book -> Back: library=%d browser=%d\n", m.toLibrary, m.toBrowser);
  std::fflush(stdout);  // a pre-r165.3 tree aborts on the assert below; this line is the verdict
  assert(m.toLibrary == 1 && m.toBrowser == 0);
  std::printf("Library -> book -> sync -> book -> Back lands on Library: library=%d\n", m.toLibrary);

  // 4b. The reader switching books from its own menu is the same session too (a replace, reader to
  //     reader, with no root name to offer).
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");
  m.replace(true, "EpubReader");
  m.noteReaderOrigin("EpubReader");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/c.epub");
  assert(m.toLibrary == 1 && m.toBrowser == 0);
  CASEOK("reader switching books from its own menu keeps the Library: library=%d\n", m.toLibrary);

  // 5. A FRESH entry from a screen that is not a root (an OPDS/plugin Details screen) must not
  //    inherit the previous book's target: the default is the file browser. The previous session
  //    ended when the reader was left for the Library.
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/a.epub");  // Back to the Library: the session ends
  assert(m.toLibrary == 1 && m.toBrowser == 0);
  m.toLibrary = m.toHome = m.toBrowser = 0;
  m.replace(false, "OpdsBookBrowser");
  m.noteReaderOrigin("OpdsBookBrowser");
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/d.epub");
  assert(m.toBrowser == 1 && m.toLibrary == 0);
  CASEOK("fresh non-root entry does not inherit a stale target: browser=%d\n", m.toBrowser);

  // 5b. Leaving the reader for Home (hold) also ends the session: a book opened later from a plugin
  //     details screen must not go back to the Library of the earlier book.
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");
  m.replace(true, "EpubReader");
  m.replace(false, "Home");  // hold-Back from the reader
  assert(m.toLibrary == 0 && m.toBrowser == 0);
  m.replace(false, "OpdsBookBrowser");
  m.noteReaderOrigin("OpdsBookBrowser");
  m.leaveReaderToOrigin("/books/e.epub");
  assert(m.toBrowser == 1 && m.toLibrary == 0);
  CASEOK("leaving the reader for Home ends the session: browser=%d\n", m.toBrowser);

  // 6. Library -> book -> sleep -> wake -> Back. goToSleep() REPLACES the reader with SleepActivity
  //    and the standby resume re-enters the SAME book (main.cpp's resume calls
  //    goToReader(openEpubPath)), so sleep is the session passing through a non-reader screen, not
  //    leaving the book.
  //    PRE-FIX VERDICT: sleep ended the session, the resume's noteReaderOrigin("Sleep") matched no
  //    root with the session closed, and the target reset to the default -- Back from the woken book
  //    went to the file browser (library=0 browser=1).
  reset(m);
  m.replace(false, "Library");
  m.noteReaderOrigin("Library");      // Library -> book
  m.replace(true, "EpubReader");      // the book is in front
  m.replace(false, "Sleep");          // goToSleep(): the sleep screen replaces the reader
  m.noteReaderOrigin("Sleep");        // the resume re-enters the same book
  m.replace(true, "EpubReader");
  m.leaveReaderToOrigin("/books/g.epub");
  CASEOK("   Library -> book -> sleep -> wake -> Back: library=%d browser=%d\n", m.toLibrary, m.toBrowser);
  std::fflush(stdout);  // the verdict line must survive the assert on a pre-r165.6 tree
  assert(m.toLibrary == 1 && m.toBrowser == 0);
  std::printf("Library -> book -> sleep -> wake -> Back lands on Library: library=%d\n", m.toLibrary);

  // 7. No origin ever recorded (a boot-time resume): the file browser, as before.
  reset(m);
  m.leaveReaderToOrigin("/books/f.epub");
  assert(m.toBrowser == 1 && m.toLibrary == 0 && m.toHome == 0);
  CASEOK("no origin recorded (boot-time resume) -> file browser: browser=%d\n", m.toBrowser);
  assert(cases == kCases);
  std::printf("back_key (reader return): %d/%d contracts OK\n", cases, kCases);
}
"""
)

# --- source-level wiring assertions -------------------------------------------------------------
settings_list = (ROOT / "src/activities/settings/SettingsListActivity.cpp").read_text()
reader_utils = (ROOT / "src/activities/reader/ReaderUtils.h").read_text()
epub_header = (ROOT / "src/activities/reader/EpubReaderActivity.h").read_text()
manager_header = (ROOT / "src/activities/ActivityManager.h").read_text()

assert "void SettingsListActivity::onBackButton() { finish(); }" in settings_list, (
    "SettingsListActivity::onBackButton must pop, not go Home"
)
assert "handleLongPressBack" not in epub_header, (
    "the reader must not claim the Back hold: the manager's hold = Home is the whole rule now"
)
assert "activityManager.leaveReaderToOrigin(filePath)" in reader_utils, (
    "the reader's Back tap must leave for the screen the book was opened from (ActivityManager decides)"
)
assert "goToFileBrowser" not in reader_utils.split("inline bool handleBackNavigation", 1)[1].split("}  // namespace", 1)[0], (
    "the reader must not hardcode the file browser as its one level -- that is what made a Library book two levels deep"
)
assert "goHome" not in reader_utils.split("inline bool handleBackNavigation", 1)[1].split("}  // namespace", 1)[0], (
    "the reader must not route a tap to Home"
)
assert "static constexpr unsigned long kLongPressBackHomeMs = 1200;" in manager_header, (
    "one hold threshold, owned by ActivityManager"
)
assert "noteReaderOrigin(currentActivity ? currentActivity->name : std::string());" in manager_source, (
    "goToReader must record the screen the book was opened from"
)
# D2 (r165.3): the wipe must key on the SESSION, not on isReaderActivity() -- KOReaderSyncActivity
# replaces the reader, so during a sync nothing in the chain is a reader activity. Checked AFTER the
# harness runs, so a pre-r165.3 tree reports the reader_return case's own verdict first.
def check_d2_wiring():
    assert "if (!isReaderActivity()) readerReturnTarget" not in manager_source, (
        "the origin wipe must not key on isReaderActivity(): a sync replaces the reader"
    )
    assert "if (!readerSessionOpen) readerReturnTarget = ReaderReturnTarget::FileBrowser;" in manager_source, (
        "noteReaderOrigin must reset the target only for a DIFFERENT session"
    )
    assert "noteReaderSessionReplaced(newActivity->isReaderActivity(), newActivity->name.c_str());" in manager_source, (
        "every replaceActivity must report the incoming screen to the session record"
    )
    assert "readerSessionOpen = false;" in leave_origin, (
        "leaving the reader for its origin must end the session"
    )


# --- scratch, INSIDE the working directory --------------------------------------------------------
# The operator's rule: nothing is written outside the working directory. This harness used to build in
# `tempfile.TemporaryDirectory()`, i.e. under %LOCALAPPDATA%\Temp, so every run scattered artifacts
# outside the repo and a failing build's generated .cpp could not be inspected afterwards. It now
# builds in _scratch/back_key_contract/ (git-ignored) and LEAVES the generated sources there on
# success: they are the exact text that was compiled, which is what a post-mortem needs.
SCRATCH = ROOT / "_scratch" / "back_key_contract"


def build(compiler, name, text):
    """Compile one generated harness against the real sources, then run it.

    A failure names the harness and the file that was kept, and prints the compiler/runner output --
    an unlabelled CalledProcessError is how r165.4's signature change went unnoticed for a round.
    """
    cpp = SCRATCH / name
    executable = SCRATCH / (name + ".exe")
    cpp.write_text(text, encoding="utf-8")
    # -I src so the generated file can #include the REAL "audio/UiSounds.h" rather than a copy of it.
    cmd = [compiler, "-std=c++20", "-Wall", "-I", str(ROOT / "src"), str(cpp), "-o", str(executable)]
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    if compiled.returncode != 0:
        raise SystemExit(
            "%s: HARNESS DOES NOT COMPILE (generated source kept at %s)\n  %s\n%s"
            % (name, cpp, " ".join(cmd), compiled.stdout + compiled.stderr)
        )
    print("$ %s" % executable.name)
    ran = subprocess.run([str(executable)], capture_output=True, text=True)
    sys.stdout.write(ran.stdout)
    sys.stdout.flush()
    if ran.returncode != 0:
        raise SystemExit("%s: FAILED (exit %d)\n%s" % (name, ran.returncode, ran.stderr))


shutil.rmtree(SCRATCH, ignore_errors=True)  # a stale .cpp must never be mistaken for this run's
SCRATCH.mkdir(parents=True, exist_ok=True)
compiler = host_compiler()
for name, text in (("back_key.cpp", harness_a), ("hold_event.cpp", harness_b), ("reader_return.cpp", harness_c)):
    build(compiler, name, text)

check_d2_wiring()

print("Back-key contract checks passed")
