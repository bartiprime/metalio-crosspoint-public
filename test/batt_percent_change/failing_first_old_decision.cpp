// BATT6 (r165.2) — THE FAILING-FIRST PROBE.
//
// Drives the r165.1 decision -- the header EXACTLY as commit 568973c9 shipped it,
// extracted from git by failing_first.sh, not a copy kept in the tree -- through the
// same model the r165.2 test uses, on the case that broke on glass:
//
//   * the FreeInkUI header band: a recorded VALUE, no addressable box;
//   * a STEADY 90 % (the value the gauge has settled on, 91 composed in the band);
//   * a whole-activity repaint of an unchanged screen, which does not drive, so the
//     record's epoch stays 0;
//   * ActivityManager resets the record at the top of every paint and the producer
//     runs on the main task while that paint is composing.
//
// r165.1 result: one whole-activity repaint request PER PASS (10000 in 10000 passes;
// on the device, paced by real paints, twelve in one session). That is the loop that
// consumed the armed return-to-Home clean.
//
// r165.2 result, same model: exactly ONE (test_batt_percent_change, first check).
//
// Exit 1 == the defect is present (this is the expected outcome for the r165.1
// header; the script inverts it so a regression here reads as a failure).
#include <cstdint>
#include <cstdio>

#include "display/BatteryPercentChange.h"

namespace {

struct OldModel {
  battpct::View view;
  battpct::Requests requests;
  uint16_t soc = 90;
  uint32_t fallbacks = 0;
  uint32_t patches = 0;

  void pass() {
    switch (battpct::need(view, soc)) {
      case battpct::Need::None:
        requests.forget();  // <-- the request memory dies on every "nothing owed" pass
        break;
      case battpct::Need::Pending:
        break;
      case battpct::Need::Fallback:
        if (requests.wantFallback(true, soc)) {
          ++fallbacks;
          paint();
        }
        break;
      case battpct::Need::Patch:
        ++patches;
        view.shownValue = static_cast<int16_t>(soc);
        view.rectEpoch = view.glassEpoch + 1;
        view.glassEpoch = view.rectEpoch;
        requests.forget();
        break;
    }
  }

  // ActivityManager: reset the record at the top, compose (the producer can pass
  // inside that window), then commit with drove=false (an unchanged frame is
  // skipped by the panel).
  void paint() {
    view.hasRect = false;
    view.rectEpoch = 0;
    pass();
    view.hasRect = true;
    view.shownValue = static_cast<int16_t>(soc);
    view.rectEpoch = 0;
  }
};

}  // namespace

int main() {
  OldModel m;
  m.view.hasRect = true;
  m.view.rectPlausible = false;  // unaddressable: the header band
  m.view.canvasIsGlass = true;
  m.view.guardsClear = true;
  m.view.shownValue = 91;  // the band composed 91; the gauge has settled to 90
  m.view.rectEpoch = 0;
  m.view.glassEpoch = 100;

  for (int i = 0; i < 10000; ++i) m.pass();

  std::printf("r165.1 decision, steady 90%%, unaddressable surface, 10000 passes:\n");
  std::printf("  whole-activity repaint requests = %u\n", m.fallbacks);
  std::printf("  in-place patches                = %u\n", m.patches);
  return m.fallbacks == 1 ? 0 : 1;  // 0 == converged (what r165.2 must do)
}
