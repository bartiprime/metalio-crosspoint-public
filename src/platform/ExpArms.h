#pragma once

// ---------------------------------------------------------------------------
// EXP1 — the experiment arm table (`exp`), r101.21.
//
// THE ONE RULE
//   Every arm is a bool that starts at a COMPILED CONSTANT whose value is
//   "what r101.20 does", and every arm lives in RAM. There is no settings key
//   and no persisted value (the only exception in the whole programme, V77-1's
//   marker file, is not in this table because that arm was not shipped).
//   A reboot re-runs static initialisation, so a reboot IS the reset — the
//   same property `standbytrim::overrideValue` already relies on.
//
// WHY THIS MAKES THE BATCH SAFE TO SHIP WITHOUT THE DEVICE
//   A disarmed arm cannot change boot, panel, rails, touch or SD-mount, because
//   the predicate it adds reads the default at every site that consults it and
//   the default is the behaviour already on the glass. The substrate is inert
//   for the same reason `src/debug/BenchDrives.*` is: with the switch off,
//   "the image behaves and logs exactly as if this file were deleted".
//
// BUILD-LEVEL KILL SWITCH
//   `-DCP_EXP=OFF` compiles the table, its strings and the `exp` command out
//   entirely (the identity-control build in the r101.21 checklist). With it
//   OFF every predicate folds to its constant, so the shipping code path is
//   instruction-for-instruction the pre-batch path.
//
// EACH ARM DEFAULTS TO TODAY'S BEHAVIOUR, AND THE DEFAULT IS NAMED "on" OR
// "off" AFTER THE BEHAVIOUR, NOT AFTER THE FLAG, so the first device command
// (`exp status`) reads as a description of the image rather than of a switch.
// --------------------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

#ifndef CP_EXP
// Host tests compile this file without the firmware's compile definitions. The
// fallback is the shipping value, so a host build exercises the same table the
// image carries.
#define CP_EXP 1
#endif

namespace exparms {

enum class Arm : uint8_t {
  // EDRV1: the row-9 completion drive at the end of a FAST paint
  // (lib/GfxRenderer/GfxRenderer.cpp). ON is today's behaviour: after a fast
  // transition the renderer appends one GL16 whole-panel drive. `exp row9 off`
  // suppresses it for this session, which is the A/B the device session runs.
  Row9Completion = 0,
  // VID1: the 8-phase short-DU table (idf/components/metalio_driver/display/
  // waveforms/my_waveform.c, built at init and never selected). OFF is today's
  // behaviour: the FAST lane drives the 20-phase DU table. `exp vid N` selects
  // the short table for the next N paints and then reverts by itself.
  VidShortDu = 1,
  // FLP1 (r101.21b): the swipe-down frontlight panel's repaint. ON is the FIXED
  // behaviour and is the shipped default: an adjustment is ONE explicit FAST/DU
  // drive with the row-9 completion suppressed, and leaving the panel arms ONE
  // from-white FULL for the repaint underneath. `exp flpanel off` restores
  // today's behaviour exactly (a bare requestUpdate() whose AUTO/policy
  // resolution may be HALF, plus the row-9 GL16, plus a HALF on drag release,
  // and no clean on exit) without a reflash.
  FrontlightPanelPartial = 2,
  Count = 3,
};

// ---- predicates the firmware calls ----------------------------------------
// EDRV1. True = the completion drive runs (r101.20). GfxRenderer consults this
// inside the existing `#if CP_RICKY_COMPLETION_DRIVE` block.
// With CP_EXP=OFF this is a constexpr constant, so the call site folds to the
// pre-batch instruction stream -- that is what the identity-control build shows.
#if CP_EXP
bool completionDrive();
// VID1. True while an `exp vid` run has frames left. The panel shim consults
// this once per paint; false is a single compare on a zero-initialised static.
bool vidShortDu();
// FLP1. True = the frontlight panel drives adjustments as one FAST/DU and cleans
// once on exit (the shipped default). False = today's behaviour.
bool panelPartialRefresh();
#else
constexpr bool completionDrive() { return true; }
constexpr bool vidShortDu() { return false; }
constexpr bool panelPartialRefresh() { return false; }
#endif

// ---- the table ------------------------------------------------------------
bool buildDefault(Arm a);   // the compiled default == r101.20's behaviour
bool todayValue(Arm a);     // what r101.20 did, written down independently
bool effective(Arm a);      // what the code path does right now
void set(Arm a, bool on);
void clear();               // every arm back to its compiled default

const char* name(Arm a);    // "row9" | "vid"
const char* help(Arm a);    // one sentence: what the arm does when armed

// One line per arm, always the same shape, greppable:
//   exp row9=on default=on (row-9 completion drive appended after a FAST paint)
// Returns the bytes written (excluding the NUL). Never truncates a line mid-arm:
// it stops at the last line that fits and NUL-terminates.
size_t status(char* out, size_t cap);

// VID1's frame countdown, owned here so `exp status` and the panel shim cannot
// disagree about how many frames are left. Called only from `exp vid` and from
// the panel shim (once per selected paint).
void vidArm(uint16_t frames);
uint16_t vidFramesRemaining();
void vidConsumeFrame();

}  // namespace exparms
