#include <cstdio>
#include <cstring>
#include "components/themes/BaseTheme.h"
#include "components/themes/andro/AndroTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "components/themes/pickyos/PickyOSTheme.h"

static int scaled(int v, MetricScale k, float s) {
  if (v == 0) return 0;
  if (k != MetricScale::Design) return v;
  return (int)lround(v * s);
}

int main(int argc, char** argv) {
  const float s = 1.35f;  // the Plus panel
  printf("| token | PickyOS | Andro | RoundedRaff | Lyra | class | PickyOS px @1.35 |\n");
  printf("|---|---|---|---|---|---|---|\n");
  for (const MetricField& f : kMetricFields) {
    const int p = PickyOSMetrics::values.*(f.member);
    const int a = AndroMetrics::values.*(f.member);
    const int r = RoundedRaffMetrics::values.*(f.member);
    const int l = LyraMetrics::values.*(f.member);
    printf("| `%s` | %d | %d | %d | %d | %s | %d |\n", f.name, p, a, r, l,
           f.scale == MetricScale::Design ? "Design" : "Physical", scaled(p, f.scale, s));
  }
  // bool fields are not in kMetricFields; print the ones PickyOS sets.
  const ThemeMetrics& m = PickyOSMetrics::values;
  printf("\nflags: listTitleBold=%d headerShowsClock=%d headerClockCentered=%d homeContinueReadingInMenu=%d "
         "keyboardCenteredText=%d popupTextBold=%d popupTextInverted=%d popupProgressDrawOutline=%d "
         "popupProgressClampPercent=%d popupProgressFillInverted=%d popupProgressOutlineInverted=%d "
         "tabPillFullSlot=%d popupTopOffsetRatio=%.3f\n",
         (int)m.listTitleBold, (int)m.headerShowsClock, (int)m.headerClockCentered,
         (int)m.homeContinueReadingInMenu, (int)m.keyboardCenteredText, (int)m.popupTextBold,
         (int)m.popupTextInverted, (int)m.popupProgressDrawOutline, (int)m.popupProgressClampPercent,
         (int)m.popupProgressFillInverted, (int)m.popupProgressOutlineInverted, (int)m.tabPillFullSlot,
         (double)m.popupTopOffsetRatio);
  (void)argv; (void)argc;
  return 0;
}
