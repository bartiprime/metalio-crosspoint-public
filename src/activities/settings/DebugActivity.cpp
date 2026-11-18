#include "DebugActivity.h"
#include "CrossPointSettings.h"

#include <cstring>

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MetalioEInk4PlusBoard.h>
#include <esp_system.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "debug/BenchDrives.h"  // BENCH1: the `benchdrives` arm behind the new row
#if CP_EXP
#include "debug/DotprodBench.h"  // SIMD1: the s8 dotprod row
#include "platform/ExpArms.h"    // EXP1: the arm rows
#endif
#include "debug/UsbShell.h"
#include <BoardConfig.h>  // the FREEINK_DEVICE_* macros the guard below reads
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include "metalio_vendor_panel.h"  // r163 3.10: read-only TPS65185 fault sample (3.9)
#endif
#include "platform/LpCoreService.h"
#include "platform/PlatformFeatures.h"
#include "platform/StandbyTrims.h"
#include "util/DebugFlag.h"

namespace fui = freeink::ui;

namespace {
// Row order is the screen order.
//
// DBG1 (r99.1): ten rows were pruned. The criterion was "the row duplicates a shell
// command, or its topic is closed":
//   Uptime / Free heap / Largest block  -> `ver` already prints all three
//   CPU clock                           -> `cpu` already prints it
//   SD log size, RTC clock, IMU sample, Touch sample -> topics closed
//   PPA self-test, PPA bench            -> PPA judged all-six-no
// The rows that stay are the ones a shell command cannot replace: the two log sinks,
// the four force-refresh probes (W22), the LP rows (LPC2), the USB shell toggle, and
// the two standby-trim arms (they are the only way to run the WAKE2 A/B without a
// rebuild). Rows are indexed by enum, so anything added from here is APPENDED.
enum Row {
  ROW_DEBUG_MODE = 0,
  ROW_PRINT_SD,
  ROW_VIEW_LOG,
  ROW_CLEAR_SCREEN,
  ROW_CLEAR_SD,
  ROW_FORCE_FULL,
  ROW_FORCE_HALF,
  ROW_FORCE_FAST,
  ROW_FORCE_GL16,
  // r86: reachability row for the LP core, the one accelerator this SoC has and
  // nothing uses yet (docs/engineering/LPCORE_AND_PPA_FRAMEWORK.md). It sits behind a
  // compile-time switch that defaults to 0; with the switch off the row must still
  // appear and report "disabled", because the point is to prove the path exists from
  // CrossPoint, not to hide it.
  ROW_LP_CORE,
  // r90: the runtime toggle for the USB debug shell. The CAPABILITY is a build
  // switch (CP_USB_SHELL, src/platform/PlatformFeatures.h); this row is what
  // makes it START, and it persists as SETTINGS.usbDebugShell ("usbDebugShell").
  // Off by default: a device that never taps it has no shell task at all.
  ROW_USB_SHELL,
  // r96: two bench rows, APPENDED (never inserted -- the indices above are
  // used in the switch below and in nothing else, but a future reader should
  // still be able to append without auditing this list).
  //   LP bench       n timed runs of the LP heartbeat program -> iteration rate
  //   LP sleep test  ONE real 60 s light-sleep slice with the LP core running
  ROW_LP_BENCH,
  ROW_LP_SLEEP,
  // r98.4: the two standby A/B arm toggles, APPENDED for the same reason as r96.
  // KEPT in r99.1 on purpose even though the trims now compile ON: they are the only
  // way to turn a trim OFF for an A/B (WAKE2: trims ON vs OFF, alternating, counting
  // `spurious wake` lines) without a rebuild. Remove them when WAKE2 closes, not before.
  //
  //   Arm PA-rail trim   standbytrim::Trim::PaRail  (CP_CUT_PA_RAIL_ON_STANDBY)
  //   Arm SD-park trim   standbytrim::Trim::SdPark  (CP_SD_PARK_ON_STANDBY)
  //   Arm CAM-rail trim  standbytrim::Trim::CamRail (CP_CUT_CAM_RAIL_ON_STANDBY)  [r101.19]
  //
  // WHY: the arm is runtime-only by design and ANY reboot wipes it. Arming them from
  // the PC means opening the USB shell, and OPENING THE SHELL RESETS THE DEVICE
  // (reset=11) -- which wipes the arm again -- so arming over USB is self-defeating.
  // This row is the only way to arm and then actually soak.
  // Second benefit: because the arm dies on a cold boot, the trim_pa=/trim_sd=
  // fields the wake-time recorder writes on every park (src/main.cpp) become a
  // COLD-BOOT DETECTOR: a park logging a value that differs from the compiled
  // default means a reboot happened since arming.
  // Neither row is persisted on purpose -- vanishing on reboot is the feature.
  ROW_TRIM_PA,
  ROW_TRIM_SD,
  // r101.19: the P0.5 (CAM_SCR camera/screen) rail arm, APPENDED beside the other two. It is
  // a SEPARATE trim on purpose: the operator may want one rail cut and the other left up
  // after the A/B, and a single "both rails" arm could not express that. Same refusals, same
  // RAM-only override, same "dies on reboot" property.
  ROW_TRIM_CAM,
  // BENCH1: the drive harness. APPENDED like everything above it. One tap runs the whole scripted
  // sequence (3 runs, ~90 s, no further input) and leaves the device on Home; the ledger is read
  // back over the USB shell / debug log as `[BENCH] drive2 ...` lines, because 45 of them do not
  // fit on a screen and the point is to compare numbers, not to read them off glass.
  // It refuses while Debug mode is OFF, which is also why it is safe to leave in the list.
  ROW_BENCH_DRIVES,
  // r101.21 EXP1: the experiment arms, APPENDED like everything above them. Three
  // rows, each one the same thing the USB shell's `exp` sub-command does -- they
  // exist because arming over USB RESETS the device (which wipes a RAM arm), so a
  // row is the only way to arm and then actually look at the glass.
  //   Arm row-9 off      EDRV1: suppress the completion drive for this session
  //   Short-DU 10 frames VID1: select the 8-phase table for the next 10 FAST paints
  //   s8 dotprod bench   SIMD1: one measurement, printed to this row and the log
  // With CP_EXP=OFF none of the three exists.
#if CP_EXP
  ROW_EXP_ROW9,
  ROW_EXP_VID,
  ROW_EXP_SIMD,
#endif
  // r163 3.10: the TPS65185 fault trace, APPENDED last so no existing index moves.
  // It is a READ-OUT, not an action: the tap takes ONE read-only sample (3.9) and the
  // value on the row is the verdict. This part's INT registers acknowledge on read, so
  // the sample is taken deliberately and stored -- never re-read on every repaint, which
  // would consume the flags before anyone could see them.
  ROW_TPS_FAULTS,
};

const char* const kRowLabels[] = {
    "Debug mode",        "Print logs to SD", "View recent log",  "Clear screen log", "Clear SD log",
    "Force full refresh", "Force half refresh", "Force fast refresh", "Force GL16 refresh", "LP core",
    "USB debug shell",   "LP bench",         "LP sleep test",    "Arm PA-rail trim",  "Arm SD-park trim",
    "Arm CAM-rail trim",
    "Drive bench 3x",
#if CP_EXP
    "Row-9 completion drive",
    "Short-DU bench 10x",
    "s8 dotprod bench",
#endif
    "TPS fault trace",
};

// Name of the waveform that was actually handed to displayBuffer(), so the log proves which drive
// ran rather than which one was requested.
const char* refreshModeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::FAST_REFRESH:
      return "FAST";
    case HalDisplay::GL16_REFRESH:
      return "GL16";
    default:
      return "?";
  }
}

// Probe body geometry. Coordinates are PERMILLE of the body box (the area below the label band,
// inside the margins), so the layout survives a different panel size. `black` is a solid fill;
// false draws a 3 px outline -- added so one frame is mass and the other is structure.
struct ProbeBlock {
  int16_t x, y, w, h;
  bool black;
};

// BEFORE and AFTER are deliberately NOT mirror images and NOT inversions. Each frame drops mass
// where the other has a hole and half-overlaps the other's edges, so residue from the previous
// frame reads as a misaligned shape (offset outline, half-covered slab) instead of landing exactly
// on the same pixels and vanishing. Blocks are ~2-4x the old 16 px checkerboard squares.
constexpr ProbeBlock kBeforeBlocks[] = {
    {20, 30, 330, 150, true},    // wide slab, top-left
    {420, 90, 170, 300, true},   // tall column right of centre
    {700, 20, 260, 110, false},  // thin frame, top-right
    {60, 260, 210, 210, false},  // square outline, left
    {480, 500, 470, 140, true},  // long bar, mid-right
    {20, 600, 190, 240, true},   // tall column, lower left
    {300, 700, 320, 140, false}, // horizontal frame, lower centre
};

constexpr ProbeBlock kAfterBlocks[] = {
    {300, 20, 300, 120, true},   // shifted right of the BEFORE slab: half overlap, half new ink
    {100, 170, 180, 340, true},  // tall column where BEFORE is empty
    {720, 300, 240, 250, true},  // block under BEFORE's thin frame
    {440, 340, 240, 100, false}, // outline crossing BEFORE's column
    {40, 560, 400, 120, true},   // low wide bar, not a copy of BEFORE's
    {560, 620, 200, 200, false}, // right-hand outline
    {240, 700, 340, 120, true},  // lower centre fill where BEFORE had an outline
};

// Gap between frames, on top of the panel's own ~650 ms drive. Long enough to see the transition,
// short enough that 11 frames finish in about ten seconds.
constexpr unsigned long kProbeDwellMs = 200;

inline bool probeStepIsAfter(const uint8_t step) { return (step & 1u) != 0; }
inline uint8_t probeStepCycle(const uint8_t step) { return step == 0 ? 0 : static_cast<uint8_t>((step + 1) / 2); }
}  // namespace

DebugActivity::DebugActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Debug", renderer, mappedInput) {}

void DebugActivity::onEnter() {
  UiListActivity::onEnter();
  viewMode_ = false;
  // Entering the page always cancels any probe left half-run by an earlier visit: a stale phase
  // would otherwise paint a BEFORE/AFTER frame here instead of the list, with no way to tell.
  resetRefreshProbe();
  refreshValues();
}

void DebugActivity::onExit() {
  // Same reason, at the other end: never hand a live probe state to the next use of this activity.
  resetRefreshProbe();
  UiListActivity::onExit();
}

void DebugActivity::resetRefreshProbe() {
  probePhase_ = 0;
  probeStep_ = 0;
  probeMode_ = 0;
  probeFrameMode_ = 0;
  probeFrameDrawn_ = false;
  probeStepMs_ = 0;
}

const char* DebugActivity::headerTitle() const {
  // The header carries the mode, so a photo of the screen says which log you are looking at.
  return viewMode_ ? "Recent log (last 16 lines)" : "Debug";
}

void DebugActivity::refreshValues() {
  // The label array and the Row enum must stay in lockstep: a missing label silently
  // shifts every row after the gap, so the screen would lie while still rendering.
  static_assert(sizeof(kRowLabels) / sizeof(kRowLabels[0]) == ITEM_COUNT,
                "kRowLabels is out of step with the Row enum / ITEM_COUNT");
  char buf[64];

  rowValues_[ROW_DEBUG_MODE] = debugflag::enabled() ? "ON" : "OFF";
  rowValues_[ROW_PRINT_SD] = actionNote_;
  rowValues_[ROW_VIEW_LOG] = "tail only";
  rowValues_[ROW_CLEAR_SCREEN] = "RTC ring";
  rowValues_[ROW_CLEAR_SD] = "";

  // r86 LP-core row. r87 turns the switch ON for the device-test image, so the row
  // value is a real PASS/FAIL: "pass d=<counter delta>", "FAIL m=<magic read back>",
  // "not built" (ULP not compiled in), or "disabled".
  // Row values are short on purpose — the value is right-aligned beside the
  // label and must not overflow it, so the detail goes to the log.
  const lpcore::Status lp = lpcore::status();
  rowValues_[ROW_LP_CORE] = lpNote_.empty() ? lpcore::stateName(lp.state) : lpNote_;
  // r90: on/off of the runtime shell, plus the two facts that explain a refusal.
  // r91: the value reports the EFFECTIVE state (persisted && session), so an operator who
  // cannot see the console can still see that a shell is holding the device awake.
  if (!usbshell::available()) {
    rowValues_[ROW_USB_SHELL] = "not built";  // CP_USB_SHELL=0
  } else if (!SETTINGS.usbDebugShell) {
    rowValues_[ROW_USB_SHELL] = "OFF";
  } else if (!usbshell::sessionEnabled()) {
    // Persisted ON, session off: cold boot, sleep, or power-off cleared it.
    rowValues_[ROW_USB_SHELL] = "ON (inactive)";
  } else {
    rowValues_[ROW_USB_SHELL] = usbshell::running() ? "ON" : "ON (start failed)";
  }

  // r96 bench rows. Each value is a SHORT outcome the operator can read off the
  // glass; the numbers live in /.crosspoint/debug.log via LOG_INF(TEST, ...).
  rowValues_[ROW_LP_BENCH] = lpBenchNote_.empty() ? "-" : lpBenchNote_;
  if (!lpSleepNote_.empty()) {
    rowValues_[ROW_LP_SLEEP] = lpSleepNote_;
  } else {
    // After a slice the value is the heartbeat delta; before one it is "-".
    const lpcore::SleepSlice& sl = lpcore::lastSleepSlice();
    if (sl.ran) {
      snprintf(buf, sizeof(buf), "hb +%lu", static_cast<unsigned long>(sl.dhb));
      lpSleepNote_ = buf;
      rowValues_[ROW_LP_SLEEP] = lpSleepNote_;
    } else {
      rowValues_[ROW_LP_SLEEP] = "-";
    }
  }

  // r98.4 trim arms. The value is the EFFECTIVE state and "(rt)" marks a runtime
  // override, because the compiled default is OFF and the operator must be able to
  // tell an arm that dies on the next reboot from one that is baked into the image.
  // A refusal replaces the value (see activateIndex): a toggle that silently does
  // nothing while reading "OFF" looks broken, not refused.
  const auto trimText = [](const standbytrim::Trim t, const std::string& note) -> std::string {
    if (!note.empty()) return note;
    const char* on = standbytrim::active(t) ? "ON" : "OFF";
    if (standbytrim::overrideValue(t) < 0) return std::string(on);
    return std::string(on) + " (rt)";
  };
  rowValues_[ROW_TRIM_PA] = trimText(standbytrim::Trim::PaRail, trimNote_[0]);
  rowValues_[ROW_TRIM_SD] = trimText(standbytrim::Trim::SdPark, trimNote_[1]);
  rowValues_[ROW_TRIM_CAM] = trimText(standbytrim::Trim::CamRail, trimNote_[2]);

  // BENCH1: the harness state, and the note from the last tap ("armed", "refused: debug off").
  {
    char st[24];
    benchdrives::statusText(st, sizeof(st));
    rowValues_[ROW_BENCH_DRIVES] = benchNote_.empty() ? st : benchNote_;
  }

#if CP_EXP
  // EXP1: the EFFECTIVE value, not the arm's flag: `row9` reads "ON" when the
  // completion drive runs, which is what the image does until the row is tapped.
  // An action note replaces the value when there is one (see activateIndex).
  rowValues_[ROW_EXP_ROW9] =
      expNote_[0].empty() ? std::string(exparms::completionDrive() ? "ON" : "OFF") : expNote_[0];
  {
    char v[24];
    const uint16_t left = exparms::vidFramesRemaining();
    snprintf(v, sizeof(v), "%u frame%s left", static_cast<unsigned>(left), left == 1 ? "" : "s");
    rowValues_[ROW_EXP_VID] = expNote_[1].empty() ? std::string(v) : expNote_[1];
  }
  rowValues_[ROW_EXP_SIMD] = expNote_[2].empty() ? std::string("-") : expNote_[2];
#endif

  // r163 3.10: the last sample's verdict. Deliberately NOT a live read: the value only
  // changes when the row is tapped, because reading the fault registers acknowledges them.
  rowValues_[ROW_TPS_FAULTS] = tpsNote_.empty() ? "tap to read" : tpsNote_;
}

void DebugActivity::openLogView() {
  const std::string logs = getLastLogs();

  // Keep the NEWEST MAX_VIEW_LINES lines: shift-in a small fixed ring rather than growing a vector,
  // so this cannot fail on a fragmented heap while the point of the screen is to debug the device.
  viewCount_ = 0;
  std::string current;
  auto push = [this](std::string&& line) {
    if (viewCount_ < MAX_VIEW_LINES) {
      viewLines_[viewCount_++] = std::move(line);
      return;
    }
    for (int i = 1; i < MAX_VIEW_LINES; ++i) viewLines_[i - 1] = std::move(viewLines_[i]);
    viewLines_[MAX_VIEW_LINES - 1] = std::move(line);
  };
  for (size_t i = 0; i < logs.size(); ++i) {
    if (logs[i] == '\n') {
      if (!current.empty()) push(std::move(current));
      current.clear();
    } else {
      current.push_back(logs[i]);
    }
  }
  if (!current.empty()) push(std::move(current));

  if (viewCount_ == 0) {
    viewLines_[0] = "(no log lines yet - enable Debug mode, reproduce, then come back)";
    viewCount_ = 1;
  }

  for (int i = 0; i < MAX_VIEW_LINES; ++i) {
    viewItems_[i] = fui::ListItem{};
    if (i < viewCount_) {
      viewItems_[i].label = viewLines_[i].c_str();
      viewItems_[i].actionValue = static_cast<int16_t>(i);
    }
  }
  viewMode_ = true;
  requestUpdate();
}

void DebugActivity::activateIndex(const int index) {
  if (viewMode_) return;  // the log view is read-only

  char note[48];
  switch (index) {
    case ROW_DEBUG_MODE: {
      const bool next = !debugflag::enabled();
      debugflag::setEnabled(next);
      logSetRuntimeDebug(next);  // the gate is what actually silences INF/DBG
      actionNote_ = next ? "logging on" : "logging off";
      break;
    }
    case ROW_PRINT_SD: {
      const size_t pending = logSdPending();
      const bool ok = logSdFlush();
      if (ok) {
        snprintf(note, sizeof(note), "wrote %u B", static_cast<unsigned>(pending));
      } else if (pending == 0) {
        snprintf(note, sizeof(note), "nothing buffered");
      } else {
        snprintf(note, sizeof(note), "write failed");
      }
      actionNote_ = note;
      break;
    }
    case ROW_VIEW_LOG:
      openLogView();
      return;
    case ROW_CLEAR_SCREEN:
      clearLastLogs();
      actionNote_ = "screen log cleared";
      break;
    case ROW_CLEAR_SD: {
      // r108 LOG6: this called logSdClear(), which removes only `debug.log` and leaves
      // `debug.log.old` behind -- while the row reads "Clear SD log", i.e. it under-promised:
      // the older generation (the one a postmortem of the PREVIOUS run needs) survived. The row
      // now uses logSdClearBoth() (r107 P7), which removes BOTH generations by name and reopens
      // an empty, header-stamped `debug.log`. It refuses while the card is unmounted, and this
      // row is only reachable with a mounted card; a refusal reads as "delete failed", never as
      // a silent half-clear.
      const bool ok = logSdClearBoth();
      actionNote_ = ok ? "SD log cleared (both generations removed)" : "delete failed (card state?)";
      break;
    }
    case ROW_FORCE_FULL:
      beginRefreshProbe(HalDisplay::FULL_REFRESH);
      return;  // the probe owns the screen; do not repaint the list over it
    case ROW_FORCE_HALF:
      beginRefreshProbe(HalDisplay::HALF_REFRESH);
      return;
    case ROW_FORCE_FAST:
      beginRefreshProbe(HalDisplay::FAST_REFRESH);
      return;
    case ROW_FORCE_GL16:
      beginRefreshProbe(HalDisplay::GL16_REFRESH);
      return;
    case ROW_LP_CORE: {
      // r87: ON DEMAND. Load, start, sample the LP core's counter twice, stop it,
      // report. Nothing starts it at boot or on a timer — this tap is the only
      // caller in the firmware.
      const lpcore::Status st = lpcore::runOnce();
      if (st.state == lpcore::State::Disabled) {
        lpNote_ = "disabled";
      } else if (st.state == lpcore::State::NotBuilt) {
        lpNote_ = "not built";
      } else if (st.pass) {
        snprintf(note, sizeof(note), "pass d=%lu", static_cast<unsigned long>(st.delta));
        lpNote_ = note;
      } else {
        snprintf(note, sizeof(note), "FAIL m=0x%lx", static_cast<unsigned long>(st.magic));
        lpNote_ = note;
      }
      break;
    }
    case ROW_LP_BENCH: {
      // r96: 8 timed runs of the heartbeat program. ~20 ms of sampling each, so
      // this holds the input loop for a fraction of a second -- the same order as
      // the r87 row above, and the detail goes to the log.
      const lpcore::Bench b = lpcore::bench(8);
      if (!b.built) {
        lpBenchNote_ = "not built";
      } else if (b.pass) {
        snprintf(note, sizeof(note), "%.1fM/s", b.rateMean / 1e6);
        lpBenchNote_ = note;
      } else {
        lpBenchNote_ = "FAIL";
      }
      break;
    }
    case ROW_LP_SLEEP: {
      // r96: arm ONE 60 s light-sleep slice. r96.2: the LP bench program is sampled,
      // STOPPED for the slice and sampled again after it -- the r96.1 version ran the
      // LP core through the sleep and the device never came back. Bounded: one HP
      // sleep call is never armed for more than 5 s.
      if (!lpcore::requestSleepSlice(60)) {
        const lpcore::SleepSlice& r = lpcore::lastSleepSlice();
        snprintf(note, sizeof(note), "no: %.20s", r.reason ? r.reason : "refused");
        lpSleepNote_ = note;
      } else {
        snprintf(note, sizeof(note), "arm %lus", static_cast<unsigned long>(lpcore::lastSleepSlice().sec));
        lpSleepNote_ = note;
      }
      break;
    }
    case ROW_USB_SHELL: {
      // r90: toggle the runtime shell. start()/stop() are the REPL lifecycle:
      // stop() deinitialises the console, frees linenoise's history and this
      // command table, and returns the USB-Serial/JTAG reader to its non-blocking
      // mode, so plain log output keeps working afterwards.
      if (!usbshell::available()) {
        actionNote_ = "not built";  // CP_USB_SHELL=0 image
        break;
      }
      // r91: the toggle acts on the EFFECTIVE state, not on the persisted byte. After a
      // cold boot / sleep / power-off the row reads "ON (inactive)", and one tap turns the
      // session on -- it does not flip a persisted ON to OFF and leave the operator with no
      // shell. R7 restores the session after a warm reset, so a reboot keeps the shell.
      const bool effective = SETTINGS.usbDebugShell != 0 && usbshell::sessionEnabled();
      if (effective) {
        SETTINGS.usbDebugShell = 0;
        SETTINGS.saveToFile();  // key "usbDebugShell"; survives the power cycle
        usbshell::disableSession();
        actionNote_ = usbshell::running() ? "stop failed" : "stopped";
      } else {
        SETTINGS.usbDebugShell = 1;
        SETTINGS.saveToFile();
        actionNote_ = usbshell::enableSession() ? "started" : "start FAILED";
      }
      LOG_INF("TEST", "usbDebugShell=%d session=%d shell_running=%d", static_cast<int>(SETTINGS.usbDebugShell),
              usbshell::sessionEnabled() ? 1 : 0, usbshell::running() ? 1 : 0);
      break;
    }
    case ROW_BENCH_DRIVES: {
      // One press, no further input. The sequence starts ~0.8 s out on the main task and its FIRST
      // step leaves this page (goHome), so the row value here is only ever seen for a moment; the
      // numbers it produces go to the log, which is the read-back channel (45 lines do not fit on
      // a screen).
      benchNote_.clear();
      if (benchdrives::running()) {
        benchNote_ = "already running";
      } else if (!debugflag::enabled()) {
        benchNote_ = "refused: debug off";
      } else if (!benchdrives::start(3)) {
        benchNote_ = "refused";
      }
      // On success the note stays empty: the sequence leaves this page in ~1 s, and a stale
      // "armed" would outlive the run and read as if it were still going.
      break;
    }
#if CP_EXP
    case ROW_EXP_ROW9: {
      // Same refusal as the shell's `exp`: the arm exists only while Debug mode is
      // on, so a row that cannot be undone cannot be reached by an accidental tap.
      expNote_[0].clear();
      if (!debugflag::enabled()) {
        expNote_[0] = "refused: debug off";
        break;
      }
      const bool next = !exparms::completionDrive();
      exparms::set(exparms::Arm::Row9Completion, next);
      LOG_INF("TEST", "exp row9 -> %s (RAM only; a reboot restores the build default)", next ? "ON" : "OFF");
      break;
    }
    case ROW_EXP_VID: {
      expNote_[1].clear();
      if (!debugflag::enabled()) {
        expNote_[1] = "refused: debug off";
        break;
      }
      exparms::vidArm(10);
      LOG_INF("TEST", "exp vid: 10 frames of the 8-phase table armed; one [VID] line per frame");
      requestUpdate();  // the next paints are the ones that take the lane
      break;
    }
    case ROW_EXP_SIMD: {
      expNote_[2].clear();
      if (!debugflag::enabled()) {
        expNote_[2] = "refused: debug off";
        break;
      }
      const dotprodb::Result r = dotprodb::run(200);
      char v[48];
      if (!r.built || !r.ok) {
        snprintf(v, sizeof(v), "kernel %s", r.built ? "values differ" : "not built");
      } else {
        snprintf(v, sizeof(v), "%.2fx (%u/%u us)", static_cast<double>(r.ratioX100) / 100.0,
                 static_cast<unsigned>(r.optUs), static_cast<unsigned>(r.ansiUs));
      }
      expNote_[2] = v;
      LOG_INF("TEST", "exp simd: kernel=%u us ansi=%u us ratio=%.2fx value=%d/%d",
              static_cast<unsigned>(r.optUs), static_cast<unsigned>(r.ansiUs),
              static_cast<double>(r.ratioX100) / 100.0, r.optResult, r.ansiResult);
      break;
    }
#endif
    case ROW_TRIM_PA:
    case ROW_TRIM_SD:
    case ROW_TRIM_CAM: {
      // r98.4: same refusals as the USB shell's `trim` handler, same words, so the
      // two paths cannot disagree about what an arm means. Parked = mid-standby: an
      // override applied there would leave the rail in a state neither arm describes.
      const standbytrim::Trim t = index == ROW_TRIM_PA   ? standbytrim::Trim::PaRail
                                  : index == ROW_TRIM_SD ? standbytrim::Trim::SdPark
                                                         : standbytrim::Trim::CamRail;
      trimNote_[0].clear();
      trimNote_[1].clear();
      trimNote_[2].clear();
      if (standbytrim::parked()) {
        // Short on screen (the value is right-aligned beside the label), full sentence
        // in the log. The row keeps reading the arm state after the next tap.
        trimNote_[static_cast<int>(t)] = "REFUSED: standby";
        LOG_INF("TEST", "trim: REFUSED -- the device is in standby (panel + rails parked). Wake it first.");
        break;
      }
      const bool next = !standbytrim::active(t);
      // RAM ONLY, deliberately: no SETTINGS field, no saveToFile(). An override that
      // survived a reboot would destroy the one property this arm is used for -- the
      // park after a cold boot must record 0 so it reads as "a reboot happened".
      // Do NOT "fix" this by persisting it.
      standbytrim::setOverride(t, next);
      char line[192];
      snprintf(line, sizeof(line),
               "trim: %s arm -> %s (runtime override; image default %s) -- applies to the NEXT standby",
               standbytrim::name(t), next ? "ON" : "OFF", standbytrim::buildDefault(t) ? "ON" : "OFF");
      standbytrim::journal(line);
      LOG_INF("TEST", "%s", line);
      break;
    }
    case ROW_TPS_FAULTS: {
      // r163 3.10: ONE read-only sample of the PMIC's fault flags (3.9). The row shows the
      // verdict; the raw values go to the log, so a photo of the screen plus the log answers
      // "which fault latched" without a console.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
      MetalioTpsFaults f{};
      const int rc = metalio_vendor_panel_tps_faults(&f, nullptr, 0);
      LOG_INF("TPS", "fault trace int1=0x%02X int2=0x%02X pg=0x%02X named=%d rc=%d (read-only sample)",
              static_cast<unsigned>(f.int1), static_cast<unsigned>(f.int2),
              static_cast<unsigned>(f.pg), f.named_bits, rc);
      if (rc != 0) {
        snprintf(note, sizeof(note), "read failed (%d)", rc);
        tpsNote_ = note;
      } else if (f.named_bits == 0) {
        tpsNote_ = "none";
      } else {
        // Short tokens from the driver's own bit map, high bit first, so this row cannot
        // disagree with the shell's decoded block.
        std::string v;
        for (int b = 7; b >= 0; --b) {
          const char* n = metalio_vendor_panel_tps_int1_short(b);
          if (n == nullptr || (f.int1 & (1u << b)) == 0) continue;
          if (!v.empty()) v += ",";
          v += n;
        }
        tpsNote_ = v + " int1";
      }
#else
      tpsNote_ = "no PMIC";
#endif
      break;
    }
    default:
      return;  // info rows do nothing
  }

  refreshValues();
  requestUpdate();
}

// #2 refresh probe. The row tap starts an automatic sequence; the frames are drawn from render(),
// NOT from the tap handler -- drawing outside the render path meant the list repainted straight over
// the first frame (the row activation calls requestUpdate() after activateIndex() returns), which is
// why the probe appeared to skip its first frame.
void DebugActivity::beginRefreshProbe(const uint8_t mode) {
  probeMode_ = mode;
  probePhase_ = 1;
  probeStep_ = 0;
  probeFrameMode_ = HalDisplay::FULL_REFRESH;  // clean from-white baseline for the reference frame
  probeFrameDrawn_ = false;
  probeStepMs_ = millis();
  // The row tap that started this probe is still latched in the input layer; swallow it, or the
  // probe's own input handler sees it and treats it as "leave the probe".
  mappedInput.discardPendingTouch();
  LOG_INF("TEST", "probe 1/%u BEFORE (FULL reference) -> %s; %u cycles follow", kProbeLastStep + 1u,
          probeModeName(), static_cast<unsigned>(kProbeCycles));
  requestUpdate();
}

// One frame per call, always scheduled by a requestUpdate() so the ~650 ms drive happens on the
// render task while the input loop keeps running. Only loop() calls this, and only once the previous
// frame has actually reached the panel (probeFrameDrawn_), so the sequence can neither outrun the
// panel nor run twice on the same step.
void DebugActivity::advanceRefreshProbe() {
  if (probeStep_ >= kProbeLastStep) {
    // End of the run: hold the last BEFORE frame on the panel (phase 2). Repainting a "done" banner
    // would cost another drive and destroy the very residue the tester is meant to look at, so the
    // frame stays and a tap (or Back) returns to the list.
    probePhase_ = 2;
    LOG_INF("TEST", "probe sequence done: %u frames, last frame held; tap or Back to exit",
            static_cast<unsigned>(kProbeLastStep) + 1u);
    return;
  }
  ++probeStep_;
  // Step 0 (the FULL reference) was set by beginRefreshProbe(); every step scheduled from here on is
  // a cycle frame and must be driven with the mode under test.
  probeFrameMode_ = probeMode_;
  probeFrameDrawn_ = false;
  requestUpdate();
}

const char* DebugActivity::probeModeName() const {
  return refreshModeName(static_cast<HalDisplay::RefreshMode>(probeMode_));
}

void DebugActivity::render(RenderLock&& lock) {
  if (probePhase_ == 0) {
    UiListActivity::render(std::move(lock));
    return;
  }
  (void)lock;  // held for the duration of this function; the raw draw below is the frame
  if (probePhase_ == 2) {
    // Sequence finished: the panel already holds the last frame and must not be driven again, or
    // the extra drive would clear the residue the tester is reading.
    return;
  }
  // Read the step ONCE: probeStep_ is only advanced by loop() after probeFrameDrawn_ is set at the
  // bottom of this function, so the label, the blocks and the drive all belong to the same frame.
  const uint8_t step = probeStep_;
  drawProbeFrame(step);
  // A probe frame is a deliberate waveform choice, and every phase depends on the mode reaching the
  // panel: the reference must be a whole-panel drive, and every cycle frame must be exactly the mode
  // under test. requestUpdate() (and the transition policy it applies) arms a one-shot promotion,
  // which GfxRenderer then substitutes for the mode passed here - so without this cancel, every
  // frame in the run drove the same auto/half waveform and the comparison was worthless.
  renderer.cancelPromotedRefresh();
  const auto mode = static_cast<HalDisplay::RefreshMode>(probeFrameMode_);
  LOG_INF("TEST", "probe frame %u/%u %s%s driven as %s", static_cast<unsigned>(step) + 1u,
          static_cast<unsigned>(kProbeLastStep) + 1u, probeStepIsAfter(step) ? "AFTER" : "BEFORE",
          step == 0 ? " (reference)" : "", refreshModeName(mode));
  renderer.displayBuffer(mode);
  probeStepMs_ = millis();
  probeFrameDrawn_ = true;  // tells loop() the next step may be scheduled
}

void DebugActivity::drawProbeFrame(const uint8_t step) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  // The reader's own font: the point of the probe is to judge the glyphs actually read on this
  // device, at the weight and size they are read at. The glyph runs below use the largest size the
  // active family offers (2x the current point size on the built-in Noto ladder); an SD .cpfont has
  // no larger slot, so there the reader face is used as-is. ensureFontLoaded() keeps a font the page
  // has not touched yet from being drawn as blanks.
  const int font = SETTINGS.getReaderFontId();
  int bigFont = builtinReaderFontVariant(font, 2.0f);
  if (bigFont == 0 || !renderer.ensureFontLoaded(bigFont)) bigFont = font;
  const int lineH = renderer.getLineHeight(font);
  const int bigLineH = renderer.getLineHeight(bigFont);
  const int bandH = 2 * lineH + 24;

  const bool after = probeStepIsAfter(step);

  renderer.clearScreen();

  // Label band: inverted in both phases, so it reads as chrome and so a mode that cannot resolve a
  // hard edge fails visibly rather than subtly. Line 1 says which half of the pair and which cycle,
  // line 2 the frame index, the mode under test and the way out -- a photo of the panel is then
  // self-describing without the log.
  renderer.fillRect(0, 0, w, bandH, true);
  char title[64];
  if (step == 0) {
    snprintf(title, sizeof(title), "%s", "BEFORE  reference");
  } else {
    snprintf(title, sizeof(title), "%s  cycle %u/%u", after ? "AFTER" : "BEFORE",
             static_cast<unsigned>(probeStepCycle(step)), static_cast<unsigned>(kProbeCycles));
  }
  renderer.drawCenteredText(font, 6, title, /*black=*/false);
  char sub[80];
  snprintf(sub, sizeof(sub), "frame %u/%u  %s%s  -  Back exits", static_cast<unsigned>(step) + 1u,
           static_cast<unsigned>(kProbeLastStep) + 1u, probeModeName(), after ? " (after)" : "");
  renderer.drawCenteredText(font, 6 + lineH + 6, sub, /*black=*/false);

  // Body box: everything below the band, inside the margins, and clear of the 6,40 48x48 back rect
  // (the band starts at y=0 and its text is centred, so only the body has to stay clear).
  const int bx0 = 24;
  const int bodyW = w - 2 * bx0;
  const int by0 = bandH + 18;
  const int bodyBottom = h - 28;
  const int bodyH = bodyBottom - by0;

  const ProbeBlock* blocks = after ? kAfterBlocks : kBeforeBlocks;
  const int blockCount = after ? static_cast<int>(sizeof(kAfterBlocks) / sizeof(kAfterBlocks[0]))
                               : static_cast<int>(sizeof(kBeforeBlocks) / sizeof(kBeforeBlocks[0]));
  for (int i = 0; i < blockCount; ++i) {
    const ProbeBlock& b = blocks[i];
    const int x = bx0 + (b.x * bodyW) / 1000;
    const int y = by0 + (b.y * bodyH) / 1000;
    const int bw = (b.w * bodyW) / 1000;
    const int bh = (b.h * bodyH) / 1000;
    if (bw < 4 || bh < 4) continue;
    if (b.black) {
      renderer.fillRect(x, y, bw, bh);
    } else {
      renderer.drawRect(x, y, bw, bh, 3, true);  // thick outline: a thin one resolves on any mode
    }
  }

  // Large glyph runs, at a DIFFERENT origin per frame (BEFORE low-left, AFTER lower and shifted
  // right), so the text residue is offset rather than printed over itself. Same words, same size:
  // what changes between the two is only where the ink sits.
  const int textX = after ? bx0 + (bodyW * 380) / 1000 : bx0 + (bodyW * 40) / 1000;
  int textY = by0 + (bodyH * (after ? 900 : 870)) / 1000;
  if (textY + 2 * bigLineH > bodyBottom) textY = bodyBottom - 2 * bigLineH;
  renderer.drawText(bigFont, textX, textY, after ? "AFTER" : "BEFORE");
  renderer.drawText(bigFont, textX, textY + bigLineH, after ? "012345678" : "ABCDEFGHIJKLM");
}

// Greedy wrap to `width`, drawn with the reader font. Returns the y it stopped at. A word that does
// not fit alone is placed anyway (len == 0 case) so the loop always advances.
int DebugActivity::drawProbeParagraph(const char* text, const int x, int y, const int width, const int font,
                                      const int lineH, const int bottom) {
  char line[256] = {0};
  size_t len = 0;
  const char* p = text;
  while (*p && y + lineH <= bottom) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char* wordEnd = p;
    while (*wordEnd && *wordEnd != ' ') ++wordEnd;
    const size_t wordLen = static_cast<size_t>(wordEnd - p);
    if (wordLen + len + 2 >= sizeof(line)) {  // pathological word: flush and try again next line
      renderer.drawText(font, x, y, line);
      y += lineH + 2;
      line[0] = 0;
      len = 0;
      continue;
    }
    char candidate[256];
    memcpy(candidate, line, len);
    size_t pos = len;
    if (pos) candidate[pos++] = ' ';
    memcpy(candidate + pos, p, wordLen);
    pos += wordLen;
    candidate[pos] = 0;
    if (len == 0 || renderer.getTextWidth(font, candidate) <= width) {
      memcpy(line, candidate, pos + 1);
      len = pos;
      p = wordEnd;
    } else {
      renderer.drawText(font, x, y, line);
      y += lineH + 2;
      line[0] = 0;
      len = 0;
    }
  }
  if (len && y + lineH <= bottom + lineH) renderer.drawText(font, x, y, line);
  return y;
}

void DebugActivity::loop() {
  if (probePhase_ != 0) {
    // The probe owns the panel, so the list underneath must not read input. Back aborts from any
    // phase; once the sequence has finished (phase 2) a tap also returns to the list.
    int tx = 0;
    int ty = 0;
    const bool tapped = mappedInput.wasScreenTapped(tx, ty);
    const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (back || (tapped && probePhase_ == 2)) {
      // Back to the list: cancel the probe outright (phase, step and tested mode) so no later
      // render can resurrect a frame, then hand the whole-screen change a scrub waveform.
      resetRefreshProbe();
      // Leaving a probe frame for the list is a whole-screen change; a scrub keeps the text edges
      // of the probe from ghosting into the rows.
      renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
      requestUpdate();
      return;
    }
    if (probePhase_ == 1) {
      // Exactly one frame per pass, and only once the render task has finished pushing the previous
      // one (~650 ms drive + kProbeDwellMs of dwell). The loop itself never waits: the scheduling is
      // a requestUpdate(), not a call into displayBuffer().
      if (probeFrameDrawn_ && (millis() - probeStepMs_) >= kProbeDwellMs) advanceRefreshProbe();
    }
    return;
  }
  if (viewMode_) {
    // Back leaves the log view and returns to the actions, rather than leaving the page entirely.
    // On the release, like every plain 'leave': a press-down answer would make the hold unreachable.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      viewMode_ = false;
      requestUpdate();
      return;
    }
  }
  UiListActivity::loop();
}

void DebugActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& theme = screen.theme();
  fui::ListProps props;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = theme.smallText;
  props.labelText.maxLines = 1;

  if (viewMode_) {
    props.items = viewItems_;
    props.count = static_cast<uint16_t>(viewCount_);
    syncListViewport(screen, props);
    screen.list(props);
    return;
  }

  for (int i = 0; i < ITEM_COUNT; ++i) {
    rowItems_[i].label = kRowLabels[i];
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
    rowItems_[i].enabled = true;
  }
  // A real switch row for the one setting on this page.
  rowItems_[ROW_DEBUG_MODE].toggle = true;
  rowItems_[ROW_DEBUG_MODE].toggleChecked = debugflag::enabled();
  rowItems_[ROW_USB_SHELL].toggle = true;
  // r91: checked only when the shell is really ON right now (persisted && session).
  rowItems_[ROW_USB_SHELL].toggleChecked = SETTINGS.usbDebugShell != 0 && usbshell::sessionEnabled();
  // r98.4: the trim rows are deliberately NOT toggle rows. freeink's list draws the
  // switch INSTEAD of the value (`if (item.toggle) ... else if (item.value)`), and the
  // value is where the arm state, the "(rt)" override marker and the refusal wording
  // have to appear -- a switch alone cannot say "OFF (rt)" or "REFUSED: standby".

  props.items = rowItems_;
  props.count = ITEM_COUNT;
  syncListViewport(screen, props);
  screen.list(props);
}