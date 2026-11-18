#include "BatteryStatsActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Row order: one block of four rows per section.
enum Row : int {
  ROW_AWAKE_TIME = 0,
  ROW_AWAKE_USED,
  ROW_AWAKE_RATE,
  ROW_AWAKE_LEFT,
  ROW_SLEEP_TIME,
  ROW_SLEEP_USED,
  ROW_SLEEP_RATE,
  ROW_SLEEP_LEFT,
  ROW_CHARGE_TIME,
  ROW_CHARGE_GAINED,
  ROW_CHARGE_RATE,
  ROW_CHARGE_LEFT,
  ROW_TOTAL_TIME,
  ROW_TOTAL_USED,
  ROW_TOTAL_RATE,
  ROW_TOTAL_LEFT,
};

const StrId kRowLabels[BatteryStatsActivity::ITEM_COUNT] = {
    StrId::STR_BATT_AWAKE,      StrId::STR_BATT_USED,    StrId::STR_BATT_PER_HOUR, StrId::STR_BATT_REMAINING,
    StrId::STR_BATT_SLEEP,      StrId::STR_BATT_USED,    StrId::STR_BATT_PER_HOUR, StrId::STR_BATT_REMAINING,
    StrId::STR_BATT_CHARGE,     StrId::STR_BATT_GAINED,  StrId::STR_BATT_PER_HOUR, StrId::STR_BATT_REMAINING,
    StrId::STR_BATT_TOTAL,      StrId::STR_BATT_USED,    StrId::STR_BATT_PER_HOUR, StrId::STR_BATT_REMAINING,
};

// Percent per hour -> "hours to empty" (discharge) or "hours to full" (charge).
// Too short a window, or a rate that rounds to nothing, means we simply cannot
// say yet: the page prints "--" rather than a made-up number.
bool estimateRemaining(const BatteryStats::Section& section, const uint16_t soc, const bool charging,
                       char* buf, const size_t size) {
  if (!BatteryStats::haveRate(section)) return false;
  const float rate = BatteryStats::perHour(section);
  float hours = 0.0f;
  if (charging) {
    if (rate <= 0.05f || soc >= 100) return false;
    hours = static_cast<float>(100 - soc) / rate;
  } else {
    if (rate >= -0.05f) return false;
    hours = static_cast<float>(soc) / (-rate);
  }
  if (hours <= 0.0f || hours > 500.0f) return false;
  BatteryStats::formatDuration(buf, size, static_cast<uint32_t>(hours * 3600.0f * 1000.0f));
  return true;
}

void formatRate(const BatteryStats::Section& section, char* buf, const size_t size) {
  if (!BatteryStats::haveRate(section)) {
    snprintf(buf, size, "--");
    return;
  }
  snprintf(buf, size, "%+.1f%%", static_cast<double>(BatteryStats::perHour(section)));
}
}  // namespace

BatteryStatsActivity::BatteryStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BatteryStats", renderer, mappedInput) {}

void BatteryStatsActivity::onEnter() {
  UiListActivity::onEnter();
  app.on(ACTION_RESET, &BatteryStatsActivity::resetTrampoline, this);
  refreshRows();
}

void BatteryStatsActivity::loop() {
  // The page is a snapshot screen: keep the accounting ticking while it is open
  // (cheap -- BatteryStats throttles itself to one gauge sample per 30 s).
  batteryStats.update();
  // The clear confirmation expires on its own: one more repaint puts the Reset
  // label back. Signed subtraction, so a millis() wrap cannot strand the notice.
  if (clearedUntilMs_ != 0 && static_cast<int32_t>(millis() - clearedUntilMs_) >= 0) {
    clearedUntilMs_ = 0;
    requestUpdate();
  }
  UiListActivity::loop();
}

void BatteryStatsActivity::render(RenderLock&& lock) {
  // THE BUG THAT MADE RESET INERT. confirmReset() armed an OptionPopup, but this
  // screen never painted one: only OptionPopup::processRender() draws the dialog
  // (every other list screen with a popup -- KOReader settings, Home button, Wi-Fi
  // -- overrides render() for exactly this reason). So a tap on Reset changed
  // nothing on the glass and left the popup ACTIVE, swallowing the next taps on its
  // invisible hit table: the button looked dead and stayed dead. Painting the popup
  // is the fix; the clear itself was already correct.
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

const char* BatteryStatsActivity::headerTitle() const {
  // Built on demand: drawChrome() runs before the list is laid out, so a buffer
  // filled in buildScreen() would be one frame stale.
  //
  // WHY THERE IS NO "CYCLE STARTED 12:04" LINE. The automatic reset is visible in the
  // three labels ("... since last charge") and in the fact that they all drop together,
  // and every cheap way to add the timestamp costs more than it pays:
  //   * this title feeds GUI.drawHeader(), a single-line themed band. A second line is
  //     new chrome drawn outside the theme, on a screen that has one line of budget;
  //   * a row subtitle (fui::ListItem::subtitle) is measured INTO the row height
  //     (list.h: subtitleHeight + labelHeight), so it would evict a row of measurements
  //     from the band -- trading a number the operator asked for against a caption;
  //   * the wall clock is often unknown: BatteryStats only trusts an epoch at or after
  //     2021 and stores 0 otherwise, so right after an unplug "12:04" is frequently
  //     unavailable, and a relative age would just restate the "Since last charge"
  //     total, which is that duration by definition.
  // So the cycle boundary is left to the labels and to the counters restarting together.
  const uint16_t soc = powerManager.getBatteryPercentage();
  snprintf(title_, sizeof(title_), "%s (%u%%)", I18N.get(StrId::STR_BATTERY_STATISTICS),
           static_cast<unsigned>(soc));
  return title_;
}

void BatteryStatsActivity::refreshRows() {
  snap_ = batteryStats.snapshot();
  char time[VALUE_LEN];
  for (int i = 0; i < ITEM_COUNT; ++i) {
    rowItems_[i].label = I18N.get(kRowLabels[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
    // Only the first row of a block carries the block name; the rest are
    // indented under it so the four sections stay visually separate.
    values_[i][0] = '\0';
    rowItems_[i].value = values_[i];
  }

  const BatteryStats::Section* sections[4] = {&snap_.awake, &snap_.sleeping, &snap_.charge, &snap_.total};
  const int firstRow[4] = {ROW_AWAKE_TIME, ROW_SLEEP_TIME, ROW_CHARGE_TIME, ROW_TOTAL_TIME};
  for (int block = 0; block < 4; ++block) {
    const BatteryStats::Section& s = *sections[block];
    const int base = firstRow[block];
    const bool charging = block == 2;

    BatteryStats::formatDuration(time, sizeof(time), s.timeMs);
    snprintf(values_[base], VALUE_LEN, "%s", time);

    snprintf(values_[base + 1], VALUE_LEN, "%+d%%", static_cast<int>(s.socDelta));

    // THE ESTIMATE COLUMN. Time and change are the RECORD (measured, since the last
    // charge -- the record restarts at every confirmed unplug, or on a manual Reset).
    // The rate and the time-left are a different thing: they answer "how fast is it
    // draining NOW", so the last block reads them from the rolling discharge window
    // instead of dividing the cycle's record by the cycle's elapsed time. That
    // division is what made the figure drift -- one bad early hour was never
    // forgotten, and the first minutes after a charge (gauge still settling at the top
    // of the curve) set the number for the rest of the session. An unready window is
    // an empty Section, so haveRate() says "cannot tell yet" with no extra branch here.
    // State blocks (awake / sleeping / charging) keep their measured per-hour: that is
    // the measured cost OF THAT STATE within this cycle, which is a record, not an
    // estimate.
    const BatteryStats::Section& rate = (block == 3) ? snap_.discharge : s;
    if (!BatteryStats::haveRate(rate)) {
      snprintf(values_[base + 2], VALUE_LEN, "--");
      snprintf(values_[base + 3], VALUE_LEN, "--");
    } else {
      formatRate(rate, values_[base + 2], VALUE_LEN);
      if (!estimateRemaining(rate, snap_.soc, charging, time, sizeof(time))) {
        snprintf(values_[base + 3], VALUE_LEN, "--");
      } else {
        snprintf(values_[base + 3], VALUE_LEN, "%s", time);
      }
    }
  }
}

void BatteryStatsActivity::buildScreen(UiScreen& screen) {
  refreshRows();

  const auto& metrics = UITheme::getInstance().getMetrics();
  // Reset button band, above the button hints.
  //
  // UI-STD-1 (docs/engineering/UI_STANDARDS.md): every interactive element's hit
  // target is at least 1.5x the theme's touch minimum. So this band is no longer a
  // hardcoded height that happens to exceed the SDK default: it resolves both rules
  // from the theme's own numbers.
  //   * the operator's sizing request: +50 % on the 62 px band that shipped in r97;
  //   * the standard's floor: ceil(1.5 x minTouchSize), which on this theme (54 px,
  //     themeTokensForLineHeight: body line height + 14) is 81 px.
  // max(93, 81) = 93 px, i.e. 1.72x of 54 -- the larger of the two constraints wins,
  // which is the point of "50% bigger than a too-small button" not being the rule.
  // The rect below is drawn AND registered from these very numbers, so the ink and
  // the hit area cannot drift apart.
  constexpr int16_t kResetBandBaseHeight = 62;  // r97 shipped height
  const int16_t touchMin = std::max<int16_t>(44, screen.theme().minTouchSize);
  const int16_t touchFloor = static_cast<int16_t>((touchMin * 3 + 1) / 2);  // ceil(1.5x)
  const int16_t kResetBandHeight =
      std::max<int16_t>(static_cast<int16_t>(kResetBandBaseHeight + kResetBandBaseHeight / 2), touchFloor);
  const int16_t resetBand = static_cast<int16_t>(kResetBandHeight + 10);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + UITheme::getInstance().getTitledHeaderHeight()), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight + resetBand), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);

  // THE FOOTER: a real touch button, registered from the rect it fills so the
  // ink and the hit area cannot drift apart.
  const int16_t screenW = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t screenH = static_cast<int16_t>(renderer.getScreenHeight());
  const fui::Rect rect{12, static_cast<int16_t>(screenH - metrics.buttonHintsHeight - kResetBandHeight - 4),
                       static_cast<int16_t>(screenW - 24), kResetBandHeight};
  fui::ButtonProps btn;
  // UI-STD-2: after a successful clear the footer says so, in the operator's own
  // language, for kClearedNoticeMs -- a silent repaint of identical-looking numbers
  // is exactly the "it does nothing" report this fixes. (A literal "Cleared" would
  // need a new I18N key in all twelve language blobs; STR_DONE is the existing
  // localized acknowledgement, and every value on the page changing to a
  // fresh-record zero is the substantive confirmation.)
  btn.label = I18N.get(clearedUntilMs_ != 0 ? StrId::STR_DONE : StrId::STR_BATT_RESET);
  btn.action = ACTION_RESET;
  btn.inputMask = fui::InputTouch;
  // The band already clears UI-STD-1's floor, so the touch minimum is the band
  // itself: ensureMinTouchRect() then cannot grow the rect past the ink (a hit
  // rect larger than the drawn button would swallow a tap on the row above it).
  btn.minTouchSize = kResetBandHeight;
  btn.text = screen.theme().smallText;
  fui::button(screen.frame(), rect, btn);
}

void BatteryStatsActivity::resetTrampoline(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<BatteryStatsActivity*>(user);
  self->confirmReset();
}

void BatteryStatsActivity::confirmReset() {
  // Confirmation FIRST: wiping the record is not undoable, and the brief asks
  // for a dialog rather than an accidental tap on the footer.
  //
  // This is the MANUAL OVERRIDE. It is not how the page normally starts a new
  // cycle -- BatteryStats zeroes the record itself at every confirmed unplug --
  // but it is the only way to clear the numbers mid-cycle, and it must keep
  // behaving exactly as it did before the automatic reset landed.
  if (optionPopup.isActive()) return;  // a second tap must not stack a second dialog
  const char* options[2] = {I18N.get(StrId::STR_YES), I18N.get(StrId::STR_CANCEL)};
  optionPopup.showMessage(I18N.get(StrId::STR_BATT_RESET), I18N.get(StrId::STR_BATT_RESET_CONFIRM), options, 2, 1,
                          [this](const int idx) {
                            if (idx != 0) {
                              requestUpdate();
                              return;
                            }
                            // batteryStats.reset() wipes the ONE persisted NVS record
                            // ("battstat"/"rec"), writes the empty one back under the
                            // nvs-write guard, and empties the in-RAM rate window, so
                            // every value this page shows -- the four blocks' time and
                            // change, the charge session, the rolling drain window --
                            // returns to a just-booted state AND a reboot cannot
                            // resurrect the old numbers.
                            batteryStats.reset();
                            // Observable consequence (UI-STD-2): the footer announces
                            // the clear for a few seconds and the rows repaint as
                            // fresh-record zeros. No other screen reads this record.
                            clearedUntilMs_ = millis() + kClearedNoticeMs;
                            LOG_INF("BATT", "stats: reset by operator (record cleared and persisted)");
                            requestUpdate();
                          });
  requestUpdate();
}

bool BatteryStatsActivity::handleCustomInput() {
  if (optionPopup.isActive()) {
    return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  }
  return false;
}

void BatteryStatsActivity::activateIndex(const int index) {
  // A row tap while the confirm dialog is open belongs to the dialog (same guard
  // every other popup host uses); acting on it here would dismiss nothing and
  // leave the page out of step with what is painted.
  if (optionPopup.isActive()) return;
  // Every row is a readout. Nothing to open, nothing to change.
  nav.selected = index;
  app.clearTapFlash();
  requestUpdate();
}
