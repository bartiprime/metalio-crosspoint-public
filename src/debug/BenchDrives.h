#pragma once

// BENCH1 — `benchdrives`, the DRIVE2 instrument.
//
// WHAT IT IS
//   A debug-only, one-press harness. Instead of the operator tapping screens in a rhythm and reading
//   the panel journal by eye, the DEVICE runs a fixed scripted sequence of activity transitions
//   itself and prints one ledger line per step:
//
//     [BENCH] drive2 run 1/3: step=settings_open drives=2 modes=DU,GL16 ms=373+166=539 EXPECT<=1
//
//   and, after the last run:
//
//     [BENCH] drive2 aggregate: total drives=62 expected=45 over=17 worst=settings_open:2
//
//   The `EXPECT<=1` column IS the assertion. The DRIVE2 question is "why does one transition cost two
//   drives", so a step with drives=2 or more is the finding; the instrument reports numbers and does
//   not editorialise.
//
// WHAT IT MEASURES, AND WHAT IT MUST NEVER DO
//   It reads the per-drive data the panel/renderer ALREADY produces — the vendor shim's
//   `[panel] show mode=.. ms=..` journal line, the same line `bench render` and the wake capture
//   consume — through the hook main.cpp already installs for every drive. It adds no timer to the
//   paint path, no second drive, and no change to any refresh decision: a step issues exactly the
//   activity transition a gesture would issue, and then only counts. If this file ever changes a
//   refresh, it stops being an instrument and becomes the thing it was built to measure.
//
// WHY 3 RUNS
//   One e-ink transition varies. A single run can lie (a slow frame folded into the next step, one
//   step that happened to be a clean). Three runs plus an aggregate is the cheapest evidence that a
//   drives=2 is the transition and not the day.
//
// SCOPE
//   Scripted (this file): the transition map rows that need no input beyond "go there".
//     home, settings list, a settings section, back out, settings list <-> library, debug open,
//     back out, home from settings, the sleep screen paint, the sleep->home resume paint, the
//     power-off dialog open/close, library again, home again.
//   The real chain those rows walk is Home -> SettingsList -> Settings (push) -> pop back to
//   SettingsList -> Home/Library. `goToSettings()` REPLACES with SettingsList, and only the
//   section push reaches the "Settings" screen, so the step table's expectations follow that:
//     settingslist_open -> SettingsList, settings_open -> Settings, settings_back -> SettingsList,
//     settingslist_reopen -> SettingsList, debug_open -> Debug, debug_back -> SettingsList.
//   A pop back onto a list whose frame is already on glass measures 0 drives BY DESIGN; those two
//   rows print "EXPECT 0-1 (pop by design...)" rather than a bare drives=0 that reads like a miss.
//   NOT scripted, by design — these need a finger, so they are measured MANUALLY and reported in the
//   same format by hand (one line per action, same columns):
//     book open (Home -> reader), page turn, chapter turn, reader menu open/close, footnote, TOC,
//     dictionary popup, mid-sync (OPDS/Wi-Fi) transitions, and a REAL standby->wake (which needs the
//     power key and drops the USB session, so it cannot be part of a run).
//   Manual example, for reference only:
//     [BENCH] drive2 manual: step=page_turn drives=2 modes=DU,GL16 ms=380+150=530 EXPECT<=1
//
// SWITCHES
//   Runtime only, behind the EXISTING debug switch (Debug > Debug mode / debugflag::enabled()).
//   With the switch off, start() refuses, loop() returns on its first compare and the journal hook
//   is not even consulted — the image behaves and logs exactly as if this file were deleted.
//   Nothing here is persisted, and no setting is added.

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class MappedInputManager;

namespace benchdrives {

// Stores the renderer/input the scripted transitions are built from (main.cpp setup).
void init(GfxRenderer& renderer, MappedInputManager& input);

// Arms the sequence. `runs` is clamped to 1..5 (default 3). Returns false when refused — debug
// mode is off, the harness is already running, or it was never initialised.
bool start(uint8_t runs = 3);

// Main task, once per loop() pass, before activityManager.loop(): issues at most one transition per
// pass, waits for the resulting drive(s) to go quiet, then prints the step's ledger line. A step
// that drives nothing or never goes quiet is logged and SKIPPED at a hard cap, so a bad step can
// never wedge the device mid-sequence.
void loop();

// Render task, from the panel journal hook (main.cpp). Only a real `[panel] show` counts; the shim
// routes other lines (rails verdict, boot clear, erase sweep) through the same hook.
void notePanelLine(const char* line);

bool running();

// Short state for the Debug row ("idle", "run 1/3", "step 4/15"). Never empty.
void statusText(char* out, size_t cap);

}  // namespace benchdrives
