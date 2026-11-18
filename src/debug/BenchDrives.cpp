#include "debug/BenchDrives.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/settings/DebugActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/SettingsSection.h"
#include "activities/util/PowerOffActivity.h"
#include "util/DebugFlag.h"

namespace benchdrives {
namespace {

// ---------------------------------------------------------------------------
// Tunables. All in ms, all chosen so the harness is a MEASUREMENT and not a race:
//   kQuietMs      after the last journal line of a step, wait this long for a SECOND drive before
//                 declaring the step over. This is the whole DRIVE2 question, so it is the one
//                 number that must not be short. 1200 ms on purpose: the manager's own entry-paint
//                 watchdog ladder re-requests a paint that produced nothing at ~1200 ms, and a
//                 step that shortens this window would CLOSE on the first drive and charge the
//                 watchdog's drive to the NEXT step -- the exact error this instrument exists to
//                 avoid. The cost is ~2 s per step and ~90 s for the default 3 runs.
//   kNoDriveMs    a step that produced no drive at all is declared done here (a screen that painted
//                 nothing). Long enough that a slow compose plus the watchdog still make it.
//   kStepCapMs    hard cap. Whatever has been counted by now is printed and the step is skipped —
//                 this is the anti-wedge, and it is why a failing step can never strand the device.
//   kStartDelayMs the shell reply must be on the wire before the first transition paints over the
//                 log stream (same reason `reboot`/`lpsleep` arm a deadline instead of acting).
constexpr uint32_t kQuietMs = 1200;
constexpr uint32_t kNoDriveMs = 2000;
constexpr uint32_t kStepCapMs = 6000;
constexpr uint32_t kStartDelayMs = 800;
constexpr uint8_t kMaxDrives = 8;
constexpr uint8_t kMaxRuns = 5;

// One step of the scripted map. `act` is what to do; `expect` is the Activity::name that should be in
// front afterwards, checked only to WARN — a step that lands somewhere unexpected is still reported
// with its numbers and then skipped. `zeroOk` marks a step whose 0-drive result is the DESIGN, not a
// miss (a pop back onto a list whose frame is already on glass), so its ledger line says so.
enum class Act : uint8_t { Home, Settings, Section, Pop, Library, Debug, Sleep, PowerOff };

struct Step {
  const char* name;
  Act act;
  const char* expect;
  bool zeroOk;
};

// THE REAL CHAIN, and why the expectations below read the way they do:
//   Home --goToSettings--> SettingsList (replaceActivity, name "SettingsList")
//   SettingsList --Act::Section (push)--> Settings (name "Settings")
//   back (pop) --> SettingsList --back--> Home/Library
// So Act::Settings lands on the LIST, and Act::Section is the one that reaches the Settings screen.
// Every `expect` here is the activity genuinely in front when the ledger line is printed.
//
// The scripted sequence. 15 of the known transitions, ordered so every visit is a different
// arrival than the one before it (a repaint onto the same screen would measure a no-op), and it
// ENDS AT HOME. The rows known to cost two drives are all here on purpose: settingslist_open,
// settings_open, library_open, debug_open, poweroff_open, sleep_paint, wake_home.
constexpr Step kSteps[] = {
    {"home_open", Act::Home, "Home", false},
    {"settingslist_open", Act::Settings, "SettingsList", false},
    {"settings_open", Act::Section, "Settings", false},
    {"settings_back", Act::Pop, "SettingsList", true},
    {"library_open", Act::Library, "Library", false},
    {"settingslist_reopen", Act::Settings, "SettingsList", false},
    {"debug_open", Act::Debug, "Debug", false},
    {"debug_back", Act::Pop, "SettingsList", true},
    {"home_from_settings", Act::Home, "Home", false},
    {"sleep_paint", Act::Sleep, "Sleep", false},
    {"wake_home", Act::Home, "Home", false},
    {"poweroff_open", Act::PowerOff, "PowerOff", false},
    {"poweroff_close", Act::Pop, "Home", false},
    {"library_again", Act::Library, "Library", false},
    {"home_from_library", Act::Home, "Home", false},
};
constexpr uint8_t kStepCount = static_cast<uint8_t>(sizeof(kSteps) / sizeof(kSteps[0]));

// ---------------------------------------------------------------------------
// The ledger. Written ONLY on the render task (notePanelLine), read ONLY on the main task, with no
// lock: `seq` is bumped around every write and every read is retried until the two seq reads match,
// so a reader can never report a half-recorded drive as a whole one. A missed read is retried on the
// next loop pass, and the settle window means there is always a next pass.
struct Ledger {
  volatile uint32_t seq;
  volatile uint32_t drives;
  volatile uint32_t msTotal;
  volatile uint32_t ms[kMaxDrives];
  char modes[kMaxDrives][20];
  // millis() of the most recent recorded drive, or of the step start when nothing has been counted.
  volatile uint32_t lastAtMs;
};

Ledger g_ledger;

GfxRenderer* g_renderer = nullptr;
MappedInputManager* g_input = nullptr;

// Written on the main task, READ on the render task (the journal hook). volatile because the point
// is that the other core sees it, not that it is fast.
volatile uint8_t g_phase = 0;  // 0 idle, 1 armed (waiting for the wire), 2 running
enum : uint8_t { kIdle = 0, kArmed = 1, kRunning = 2 };

uint8_t g_runsWanted = 3;
uint8_t g_run = 0;
uint8_t g_step = 0;
uint32_t g_armAtMs = 0;
uint32_t g_stepStartMs = 0;
bool g_stepIssued = false;

// Run totals and the aggregate, main task only.
uint32_t g_runSteps = 0, g_runDrives = 0, g_runOver = 0, g_runWorst = 0;
char g_runWorstStep[24] = {0};
uint32_t g_allSteps = 0, g_allDrives = 0, g_allOver = 0;
uint32_t g_allWorst = 0;
char g_allWorstStep[24] = {0};

void resetLedger(const uint32_t now) {
  g_ledger.seq = g_ledger.seq + 1;
  g_ledger.drives = 0;
  g_ledger.msTotal = 0;
  for (uint8_t i = 0; i < kMaxDrives; ++i) {
    g_ledger.ms[i] = 0;
    g_ledger.modes[i][0] = '\0';
  }
  g_ledger.lastAtMs = now;
  g_ledger.seq = g_ledger.seq + 1;
}

// Copies the ledger out, retrying while the render task writes. False = it changed under us; the
// caller keeps the previous (empty) values and asks again next pass.
bool readLedger(uint32_t* drives, uint32_t* msTotal, uint32_t* lastAt, uint32_t* ms, char modes[][20]) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint32_t before = g_ledger.seq;
    *drives = g_ledger.drives;
    *msTotal = g_ledger.msTotal;
    *lastAt = g_ledger.lastAtMs;
    const uint32_t n = *drives < kMaxDrives ? *drives : kMaxDrives;
    for (uint32_t i = 0; i < n; ++i) {
      ms[i] = g_ledger.ms[i];
      memcpy(modes[i], (const void*)g_ledger.modes[i], sizeof(modes[i]));
      modes[i][sizeof(modes[i]) - 1] = '\0';
    }
    if (g_ledger.seq == before) return true;
  }
  return false;
}

void goHomeIfNot() {
  // The sequence is designed to end on Home; this is only the belt-and-braces for an abort or a step
  // that failed. It runs through the ordinary transition path, so it is itself one normal paint.
  if (!activityManager.isActivityNamed("Home")) activityManager.goHome();
}

void stopAndPark(const char* why) {
  g_phase = kIdle;
  g_stepIssued = false;
  LOG_INF("BENCH", "drive2 aborted: %s (runs done=%u, steps done=%u) -- back to Home", why, g_run, g_allSteps);
  goHomeIfNot();
}

// Issues the step's transition through the SAME public ActivityManager entry points a gesture uses.
// Nothing here touches refresh policy: the transition paints exactly what it would paint for a
// finger, and the harness only watches the journal afterwards.
void issueStep(const uint8_t index) {
  const Step& s = kSteps[index];
  switch (s.act) {
    case Act::Home:
      activityManager.goHome();
      break;
    case Act::Settings:
      activityManager.goToSettings();
      break;
    case Act::Section: {
      // The same screen SettingsListActivity opens on a row tap (Display is the first section).
      auto a = makeUniqueNoThrow<SettingsActivity>(*g_renderer, *g_input, SettingsSection::Display);
      if (a) {
        activityManager.pushActivity(std::move(a));
      } else {
        LOG_ERR("BENCH", "drive2 warn: step=%s OOM building the section screen", s.name);
      }
      break;
    }
    case Act::Pop:
      activityManager.popActivity();
      break;
    case Act::Library:
      activityManager.goToLibrary();
      break;
    case Act::Debug: {
      auto a = makeUniqueNoThrow<DebugActivity>(*g_renderer, *g_input);
      if (a) {
        activityManager.pushActivity(std::move(a));
      } else {
        LOG_ERR("BENCH", "drive2 warn: step=%s OOM building Debug", s.name);
      }
      break;
    }
    case Act::Sleep:
      // The SLEEP SCREEN transition (replaceActivity -> SleepActivity paints from onEnter), not a
      // standby: a real standby needs the power key to wake and drops the USB session, so it is a
      // manual measurement (see the header). This is the paint half, which is the half that drives.
      activityManager.replaceActivity(std::make_unique<SleepActivity>(*g_renderer, *g_input, false));
      break;
    case Act::PowerOff: {
      auto a = makeUniqueNoThrow<PowerOffActivity>(*g_renderer, *g_input);
      if (a) {
        activityManager.pushActivity(std::move(a));
      } else {
        LOG_ERR("BENCH", "drive2 warn: step=%s OOM building the dialog", s.name);
      }
      break;
    }
  }
}

// One ledger line, in the fixed shape the tables are compared across sessions with.
void logStep(const Step& step, const uint32_t drives, const uint32_t* ms, char modes[][20]) {
  const char* name = step.name;
  char modeList[96];
  char msList[96];
  size_t mp = 0;
  size_t sp = 0;
  modeList[0] = '\0';
  msList[0] = '\0';
  if (drives == 0) {
    snprintf(modeList, sizeof(modeList), "-");
    snprintf(msList, sizeof(msList), "0");
  } else {
    for (uint32_t i = 0; i < drives && i < kMaxDrives; ++i) {
      mp += snprintf(modeList + mp, sizeof(modeList) - mp, "%s%s", i ? "," : "", modes[i]);
      sp += snprintf(msList + sp, sizeof(msList) - sp, "%s%lu", i ? "+" : "",
                     static_cast<unsigned long>(ms[i]));
      if (mp >= sizeof(modeList) - 8 || sp >= sizeof(msList) - 12) break;
    }
  }
  uint32_t total = 0;
  for (uint32_t i = 0; i < drives && i < kMaxDrives; ++i) total += ms[i];
  if (drives == 0) {
    // A 0-drive line for a pop step is the design: the list frame is already on glass, so say so
    // instead of printing a bare drives=0 that reads like a miss.
    LOG_INF("BENCH", "drive2 run %u/%u: step=%s drives=0 modes=%s ms=%s %s",
            static_cast<unsigned>(g_run), static_cast<unsigned>(g_runsWanted), name, modeList, msList,
            step.zeroOk ? "EXPECT 0-1 (pop by design: list frame already on glass, not a failure)"
                        : "EXPECT<=1");
  } else {
    char sum[112];
    snprintf(sum, sizeof(sum), "%s=%lu", msList, static_cast<unsigned long>(total));
    LOG_INF("BENCH", "drive2 run %u/%u: step=%s drives=%lu modes=%s ms=%s EXPECT<=1",
            static_cast<unsigned>(g_run), static_cast<unsigned>(g_runsWanted), name,
            static_cast<unsigned long>(drives), modeList, sum);
  }
}

void noteWorst(char* step, uint32_t* worst, const char* name, const uint32_t drives) {
  if (drives > *worst) {
    *worst = drives;
    snprintf(step, 24, "%s", name);
  }
}

void finishRun() {
  char worst[40];
  if (g_runWorst == 0) {
    snprintf(worst, sizeof(worst), "-");
  } else {
    snprintf(worst, sizeof(worst), "%s:%lu", g_runWorstStep, static_cast<unsigned long>(g_runWorst));
  }
  LOG_INF("BENCH", "drive2 run %u/%u summary: steps=%lu drives=%lu over=%lu worst=%s",
          static_cast<unsigned>(g_run), static_cast<unsigned>(g_runsWanted),
          static_cast<unsigned long>(g_runSteps), static_cast<unsigned long>(g_runDrives),
          static_cast<unsigned long>(g_runOver), worst);
  // Aggregate across runs, worst kept by drive count so it names the row that is still costing two.
  g_allSteps += g_runSteps;
  g_allDrives += g_runDrives;
  g_allOver += g_runOver;
  if (g_runWorst > g_allWorst) {
    g_allWorst = g_runWorst;
    snprintf(g_allWorstStep, sizeof(g_allWorstStep), "%s", g_runWorstStep);
  }
  g_runSteps = g_runDrives = g_runOver = g_runWorst = 0;
  g_runWorstStep[0] = '\0';
}

void finishAll() {
  char worst[40];
  if (g_allWorst == 0) {
    snprintf(worst, sizeof(worst), "-");
  } else {
    snprintf(worst, sizeof(worst), "%s:%lu", g_allWorstStep, static_cast<unsigned long>(g_allWorst));
  }
  LOG_INF("BENCH",
          "drive2 aggregate: total drives=%lu expected=%lu over=%lu worst=%s runs=%u steps=%lu",
          static_cast<unsigned long>(g_allDrives), static_cast<unsigned long>(g_allSteps),
          static_cast<unsigned long>(g_allOver), worst, static_cast<unsigned>(g_runsWanted),
          static_cast<unsigned long>(g_allSteps));
  LOG_INF("BENCH", "drive2 done: instrument disarmed; the manual rows (page turn, book open, reader menu,"
                   " real standby->wake) are measured by hand in the same format");
  g_phase = kIdle;
  g_stepIssued = false;
}

// Called after each step's ledger line: book-keeping, then either the next step, the next run, or the
// end. Returns with g_phase == kIdle when the whole sequence is over.
void advance(uint32_t drives) {
  const char* name = kSteps[g_step].name;
  ++g_runSteps;
  g_runDrives += drives;
  if (drives > 1) ++g_runOver;
  noteWorst(g_runWorstStep, &g_runWorst, name, drives);
  ++g_step;
  g_stepIssued = false;
  if (g_step < kStepCount) return;

  finishRun();
  ++g_run;
  g_step = 0;
  if (g_run > g_runsWanted) {
    finishAll();
    goHomeIfNot();  // no-op when the last step already landed Home
  }
}

}  // namespace

void init(GfxRenderer& renderer, MappedInputManager& input) {
  g_renderer = &renderer;
  g_input = &input;
}

bool start(uint8_t runs) {
  // THE SWITCH. Nothing arms, nothing counts and nothing logs unless debug mode is on.
  if (!debugflag::enabled()) {
    LOG_DBG("BENCH", "benchdrives refused: debug mode is off");
    return false;
  }
  if (g_renderer == nullptr || g_input == nullptr) return false;
  if (g_phase != kIdle) return false;
  if (runs < 1) runs = 1;
  if (runs > kMaxRuns) runs = kMaxRuns;

  g_runsWanted = runs;
  g_run = 1;
  g_step = 0;
  g_stepIssued = false;
  g_runSteps = g_runDrives = g_runOver = g_runWorst = 0;
  g_runWorstStep[0] = '\0';
  g_allSteps = g_allDrives = g_allOver = 0;
  g_allWorst = 0;
  g_allWorstStep[0] = '\0';
  resetLedger(millis());
  g_armAtMs = millis() + kStartDelayMs;
  g_phase = kArmed;
  LOG_INF("BENCH", "drive2 arm: %u run(s) x %u scripted steps, %lu ms settle window, start in %lu ms",
          static_cast<unsigned>(runs), static_cast<unsigned>(kStepCount),
          static_cast<unsigned long>(kQuietMs), static_cast<unsigned long>(kStartDelayMs));
  return true;
}

bool running() { return g_phase != kIdle; }

void statusText(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  if (g_phase == kIdle) {
    snprintf(out, cap, "idle");
  } else if (g_phase == kArmed) {
    snprintf(out, cap, "arm %u/%u", static_cast<unsigned>(g_run), static_cast<unsigned>(g_runsWanted));
  } else {
    snprintf(out, cap, "run %u/%u s%u", static_cast<unsigned>(g_run),
             static_cast<unsigned>(g_runsWanted), static_cast<unsigned>(g_step) + 1u);
  }
}

void notePanelLine(const char* line) {
  if (g_phase != kRunning || line == nullptr) return;
  // ONLY A REAL DRIVE. The shim journals several other lines through the same hook (boot clear,
  // the rails/PWR_GOOD verdict, the erase sweep); LOG1 marks a no-drive show() as
  // `show SKIPPED (no drive)`. Counting either would turn this instrument into a liar.
  if (strncmp(line, "[panel] show", 12) != 0) return;
  if (strstr(line, "SKIPPED") != nullptr) return;
  const char* modeTok = strstr(line, "mode=");
  if (modeTok == nullptr) return;
  modeTok += 5;
  const char* msTok = strstr(line, "ms=");
  const uint32_t ms = msTok != nullptr ? static_cast<uint32_t>(strtoul(msTok + 3, nullptr, 10)) : 0u;

  const uint32_t n = g_ledger.drives;
  if (n < kMaxDrives) {
    size_t len = 0;
    while (modeTok[len] != '\0' && modeTok[len] != ' ' && len + 1 < sizeof(g_ledger.modes[0])) ++len;
    memcpy(g_ledger.modes[n], modeTok, len);
    g_ledger.modes[n][len] = '\0';
    g_ledger.ms[n] = ms;
  }
  g_ledger.msTotal = g_ledger.msTotal + ms;
  g_ledger.drives = n + 1;
  g_ledger.lastAtMs = millis();
  g_ledger.seq = g_ledger.seq + 1;
}

void loop() {
  if (g_phase == kIdle) return;
  // The switch is checked on EVERY pass, not only at start: turning Debug mode off mid-run must stop
  // the harness, and it must do so silently (the log gate is already closed by then).
  if (!debugflag::enabled()) {
    g_phase = kIdle;
    g_stepIssued = false;
    goHomeIfNot();
    return;
  }
  if (g_renderer == nullptr || g_input == nullptr) {
    g_phase = kIdle;
    return;
  }
  // USB drive mode detaches the filesystem and owns the loop; never script transitions through it.
  if (activityManager.requiresExclusiveStorageLoop()) return;

  const uint32_t now = millis();
  if (g_phase == kArmed) {
    if (static_cast<int32_t>(now - g_armAtMs) < 0) return;
    g_phase = kRunning;
    g_stepIssued = false;
    return;
  }

  if (!g_stepIssued) {
    // Snapshot BEFORE the transition so the count covers this step and nothing before it.
    resetLedger(now);
    g_stepStartMs = now;
    issueStep(g_step);
    g_stepIssued = true;
    return;  // one transition per pass; activityManager.loop() acts on it further down this pass
  }

  uint32_t drives = 0, msTotal = 0, lastAt = now;
  uint32_t ms[kMaxDrives];
  char modes[kMaxDrives][20];
  for (uint8_t i = 0; i < kMaxDrives; ++i) {
    ms[i] = 0;
    modes[i][0] = '\0';
  }
  if (!readLedger(&drives, &msTotal, &lastAt, ms, modes)) return;

  const uint32_t elapsed = now - g_stepStartMs;
  const bool quiet = static_cast<int32_t>(now - lastAt) >= static_cast<int32_t>(kQuietMs);
  const bool done = (drives > 0 && quiet) || (drives == 0 && elapsed >= kNoDriveMs) || elapsed >= kStepCapMs;
  if (!done) return;

  const Step& s = kSteps[g_step];
  if (elapsed >= kStepCapMs) {
    LOG_INF("BENCH", "drive2 warn: step=%s hit the %lu ms cap with drives=%lu (counted what was seen)",
            s.name, static_cast<unsigned long>(kStepCapMs), static_cast<unsigned long>(drives));
  }
  if (s.expect != nullptr && !activityManager.isActivityNamed(s.expect)) {
    LOG_INF("BENCH", "drive2 warn: step=%s expected %s in front, it is not", s.name, s.expect);
  }
  logStep(s, drives, ms, modes);
  advance(drives);
}

}  // namespace benchdrives
