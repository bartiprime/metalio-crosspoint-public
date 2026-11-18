// Host-side, falsifiable checks for the PickyOS token table and the design
// language it claims to implement. Fails loudly; exit code is the verdict.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <cstdio>
#include <vector>
#include "components/themes/BaseTheme.h"
#include "components/themes/andro/AndroTheme.h"
#include "components/themes/pickyos/PickyOSTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

static int failures = 0;
static void check(bool ok, const char* what, int got, int want) {
  if (!ok) {
    printf("FAIL  %-46s got=%d want=%d\n", what, got, want);
    ++failures;
  } else {
    printf("ok    %-46s %d\n", what, got);
  }
}
static int sc(int v, MetricScale k, float s) {
  if (v == 0) return 0;
  return k == MetricScale::Design ? (int)lround(v * s) : v;
}

int main() {
  const ThemeMetrics& m = PickyOSMetrics::values;
  const float s = 1.35f;

  // 1. ONE stroke weight: every ink width in the table is the same 2 px.
  for (const MetricField& f : kMetricFields) {
    const char* n = f.name;
    const bool isStroke = !strcmp(n, "headerRuleThickness") || !strcmp(n, "chevronStroke") ||
                          !strcmp(n, "homeFeaturedCoverBorder") || !strcmp(n, "homeFeaturedProgressBarStroke") ||
                          !strcmp(n, "homeScrollIndicatorWidth") || !strcmp(n, "listScrollWidth") ||
                          !strcmp(n, "textFieldNormalThickness") || !strcmp(n, "popupFrameThickness");
    if (isStroke) check(m.*(f.member) == 2, n, m.*(f.member), 2);
  }

  // 2. Tight corners: no pill, no stadium sentinel, nothing above 8 design units.
  check(m.listRowRadius <= 8, "listRowRadius <= 8 (not a pill)", m.listRowRadius, 0);
  check(m.homeFeaturedCoverRadius <= 8, "homeFeaturedCoverRadius <= 8", m.homeFeaturedCoverRadius, 6);
  check(m.popupCornerRadius <= 8, "popupCornerRadius <= 8", m.popupCornerRadius, 0);
  check(m.controlRadius <= 8, "controlRadius <= 8", m.controlRadius, 4);
  check(m.sheetRadius <= 8, "sheetRadius <= 8", m.sheetRadius, 8);
  check(m.capsuleRadius != 255, "capsuleRadius is not the stadium sentinel", m.capsuleRadius, 4);
  check(m.controlRadius != RoundedRaffMetrics::values.controlRadius, "controlRadius differs from RoundedRaff",
        m.controlRadius, RoundedRaffMetrics::values.controlRadius);

  // 3. Fill-free selection vocabulary.
  check(m.listSelectionStyle == 2, "listSelectionStyle == underline", m.listSelectionStyle, 2);
  check(!m.listTitleBold, "listTitleBold == false", (int)m.listTitleBold, 0);

  // 4. Ink floor: nothing that lands on the panel is thinner than 2 px there.
  for (const MetricField& f : kMetricFields) {
    const int v = m.*(f.member);
    const bool isInk = !strcmp(f.name, "headerRuleThickness") || !strcmp(f.name, "chevronStroke") ||
                       !strcmp(f.name, "homeScrollIndicatorWidth") || !strcmp(f.name, "listScrollWidth");
    if (isInk) check(v >= 2, f.name, v, 2);
  }

  // 5. The theme class is concrete: every pure virtual of BaseTheme is
  //    overridden (a missing override is a compile error here, not a crash on
  //    the glass). The knobs themselves are single-line returns; the registration
  //    meta-check (registration.py) reads them out of the header.
  static_assert(!std::is_abstract_v<PickyOSTheme>, "PickyOSTheme must implement every BaseTheme pure virtual");
  static_assert(std::is_base_of_v<BaseTheme, PickyOSTheme>, "PickyOSTheme must derive from BaseTheme");
  static_assert(sizeof(PickyOSTheme) == sizeof(BaseTheme), "PickyOSTheme must add no state (metrics are the theme)");
  printf("ok    PickyOSTheme is concrete, stateless, and a BaseTheme\n");

  // 6. Home geometry on the Plus panel (684x1216), derived the way
  //    HomeActivity derives it: band top = homeTopPadding + tile + offset.
  const int panelH = 1216, panelW = 684;
  const int side = sc(m.contentSidePadding, MetricScale::Design, s);
  const int bandTop = sc(m.homeTopPadding, MetricScale::Design, s) +
                      sc(m.homeCoverTileHeight, MetricScale::Physical, s) +
                      sc(m.homeMenuTopOffset, MetricScale::Design, s);
  const int rowH = sc(m.menuRowHeight, MetricScale::Design, s);
  const int usable = panelH - bandTop - 24;
  const int step = std::clamp(usable / 5, rowH + sc(m.menuSpacing, MetricScale::Design, s), rowH + rowH / 2);
  const int blockH = 4 * step + rowH;
  printf("\n-- Home on the Plus panel (684x1216, scale %.2f) --\n", s);
  printf("content rail          %d px\n", side);
  printf("status band           %d px (rule %d px at its bottom edge)\n",
         sc(m.headerHeight, MetricScale::Design, s), sc(m.headerRuleThickness, MetricScale::Physical, s));
  printf("featured card band    y=%d h=%d\n", sc(m.homeTopPadding, MetricScale::Design, s),
         sc(m.homeCoverTileHeight, MetricScale::Physical, s));
  printf("cover art             %dx%d, frame %d, radius %d\n", m.homeFeaturedCoverLargeWidth,
         m.homeFeaturedCoverLargeHeight, m.homeFeaturedCoverBorder,
         sc(m.homeFeaturedCoverRadius, MetricScale::Design, s));
  printf("menu band top         y=%d, usable %d\n", bandTop, usable);
  printf("menu row / pitch      %d / %d px, block %d px, top %d\n", rowH, step, blockH,
         bandTop + std::max(0, (usable - blockH) / 2));
  printf("list row pitch        %d px\n", sc(m.listRowHeight, MetricScale::Design, s));

  // Card must fit its band: art + frame + air, never over.
  const int cardNeeded = m.homeFeaturedCoverLargeHeight + 2 * m.homeFeaturedCoverBorder;
  check(cardNeeded <= m.homeCoverTileHeight, "featured art+frame fits the card band", cardNeeded,
        m.homeCoverTileHeight);
  // The thumb cache key must equal the painted height, or the blit rescales.
  check(m.homeCoverHeight == m.homeFeaturedCoverLargeHeight, "thumb key == painted cover height", m.homeCoverHeight,
        m.homeFeaturedCoverLargeHeight);

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL CHECKS PASSED", failures);
  return failures ? 1 : 0;
}
