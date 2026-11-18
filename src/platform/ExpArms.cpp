// EXP1 — see src/platform/ExpArms.h for the contract.
//
// Everything here is RAM and static-initialised. There is no settings key and no
// file: a reboot restores every arm to its compiled default, which is the
// behaviour the shipping image already had.

#include "platform/ExpArms.h"

#include <cstdio>
#include <cstring>

#if __has_include("metalio_vendor_panel.h")
// Firmware build: the panel shim owns the short-DU frame countdown, because it
// is the only place that can select the table for a drive.
#define EXP_ARMS_PANEL 1
#include "metalio_vendor_panel.h"
#else
// Host build (test/exp_arms): the driver is not on the include path, so the
// countdown is mirrored here. The host test asserts the DEFAULTS and the status
// format, never the live countdown.
#define EXP_ARMS_PANEL 0
#endif

#if CP_EXP

namespace exparms {
namespace {

// ---- the arms -------------------------------------------------------------
// Each initialiser is a constant and is the value r101.20 shipped with.
bool g_row9Completion = true;  // EDRV1: the completion drive runs (today)
// FLP1: SHIPPED ARMED. This is the one arm in the table whose default is NOT
// r101.20's behaviour -- the operator asked for the panel bug fixed in this
// image, and `exp flpanel off` is the one-command rollback. See ExpArms.h.
bool g_panelPartial = true;
#if !EXP_ARMS_PANEL
uint16_t g_vidRemaining = 0;   // VID1: no frames armed (today)
#endif

struct ArmDef {
  const char* name;
  const char* help;
  bool ArmDefDefault;  // the compiled default, kept beside the state below
  bool today;          // what r101.20 did -- written down independently
};

// Order must match Arm. A new arm is APPENDED and gets its defaults here.
const ArmDef kArms[] = {
    {"row9",
     "row-9 completion drive appended after a FAST paint (EDRV1); `off` suppresses it",
     true, true},
    {"vid",
     "8-phase short-DU table for the next N paints (VID1); `exp vid N` selects it",
     false, false},
    {"flpanel",
     "frontlight panel: adjust = one FAST/DU drive (no completion), exit = one clean from white; `off` restores today's repaints",
     true, false},
};
static_assert(sizeof(kArms) / sizeof(kArms[0]) == static_cast<size_t>(Arm::Count),
              "one ArmDef per Arm");

int idx(const Arm a) { return static_cast<int>(a); }

bool state(const Arm a) {
  switch (a) {
    case Arm::Row9Completion:
      return g_row9Completion;
    case Arm::VidShortDu:
#if EXP_ARMS_PANEL
      return metalio_vendor_panel_short_du_remaining() > 0;
#else
      return g_vidRemaining > 0;
#endif
    case Arm::FrontlightPanelPartial:
      return g_panelPartial;
    default:
      return false;
  }
}

}  // namespace

bool completionDrive() {
#if CP_EXP
  return g_row9Completion;
#else
  // The substrate is compiled out: fold to the constant r101.20 had.
  return true;
#endif
}

bool vidShortDu() { return state(Arm::VidShortDu); }

bool panelPartialRefresh() { return state(Arm::FrontlightPanelPartial); }

bool buildDefault(const Arm a) { return kArms[idx(a)].ArmDefDefault; }
bool todayValue(const Arm a) { return kArms[idx(a)].today; }
bool effective(const Arm a) { return state(a); }

void set(const Arm a, const bool on) {
  switch (a) {
    case Arm::Row9Completion:
      g_row9Completion = on;
      break;
    case Arm::VidShortDu:
#if EXP_ARMS_PANEL
      metalio_vendor_panel_short_du_frames(on ? 1 : 0);
#else
      g_vidRemaining = on ? 1 : 0;
#endif
      break;
    case Arm::FrontlightPanelPartial:
      g_panelPartial = on;
      break;
    default:
      break;
  }
}

void clear() {
  g_row9Completion = true;
  g_panelPartial = true;
#if EXP_ARMS_PANEL
  metalio_vendor_panel_short_du_frames(0);
#else
  g_vidRemaining = 0;
#endif
}

const char* name(const Arm a) { return kArms[idx(a)].name; }
const char* help(const Arm a) { return kArms[idx(a)].help; }

size_t status(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
#if !CP_EXP
  // Compiled out: there is nothing to report and no string to report it with.
  out[0] = '\0';
  return 0;
#else
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < sizeof(kArms) / sizeof(kArms[0]); ++i) {
    const Arm a = static_cast<Arm>(i);
    // `remaining=` is only meaningful for the arm that owns a countdown; for
    // every other arm the tail of the line is empty, so the shape stays
    // "exp <name>=<on|off> default=<on|off> (<what it does>)" and one grep finds
    // every arm.
    char tail[32];
    tail[0] = '\0';
    if (a == Arm::VidShortDu) {
      snprintf(tail, sizeof(tail), " remaining=%u", static_cast<unsigned>(vidFramesRemaining()));
    }
    char line[224];
    const int n = snprintf(line, sizeof(line), "exp %s=%s default=%s (%s)%s", kArms[i].name,
                           effective(a) ? "on" : "off", kArms[i].ArmDefDefault ? "on" : "off",
                           kArms[i].help, tail);
    if (n <= 0) continue;
    const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
    if (used + len + 1 >= cap) break;  // never split a line
    std::memcpy(out + used, line, len);
    used += len;
    out[used++] = '\n';
    out[used] = '\0';
  }
  return used;
#endif
}

void vidArm(const uint16_t frames) {
#if EXP_ARMS_PANEL
  metalio_vendor_panel_short_du_frames(static_cast<int>(frames));
#else
  g_vidRemaining = frames;
#endif
}

uint16_t vidFramesRemaining() {
#if EXP_ARMS_PANEL
  const int r = metalio_vendor_panel_short_du_remaining();
  return r > 0 ? static_cast<uint16_t>(r) : static_cast<uint16_t>(0);
#else
  return g_vidRemaining;
#endif
}

void vidConsumeFrame() {
#if EXP_ARMS_PANEL
  // The panel shim decrements its own countdown as it drives.
#else
  if (g_vidRemaining > 0) --g_vidRemaining;
#endif
}

}  // namespace exparms

#endif  // CP_EXP
