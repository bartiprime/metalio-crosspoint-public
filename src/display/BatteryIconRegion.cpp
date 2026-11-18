#include "display/BatteryIconRegion.h"

#include "display/RefreshPolicy.h"

#if CP_ICON_PARTIAL_REPAINT

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace {

// The rectangle the LAST paint of the visible screen drew the glyph into, in
// logical screen coordinates. Written by BaseTheme::drawBatteryOutline(), cleared
// by cpBatteryIconBeginPaint() at the top of every activity paint -- so a screen
// that does not draw the icon cannot leave a stale box behind for the next one.
int s_x = 0;
int s_y = 0;
int s_w = 0;
int s_h = 0;
bool s_valid = false;

// Design-space body is 15-16 x 12 px (ThemeMetrics::batteryWidth/Height, scaled to
// panel pixels by the density table), and the nub adds one column. The bounds are
// deliberately loose -- they exist to reject a garbage record (an uninitialised or
// absurd box), not to re-derive the theme's geometry, which is what recording at
// the draw site avoids.
constexpr int kMinIconSide = 4;
constexpr int kMaxIconSide = 64;

// Edge state for the charging-indicator repaint (BATT1R/BATT2).
//
// hasBaseline suppresses the first observation after boot/resume: a fresh device has
// nothing to repaint, and treating "no previous value" as an edge would drive the icon on
// the first loop after every wake. lastCharging is the state the GLASS is believed to
// show -- so it is only advanced by something that actually reached the glass: a
// successful drive below, or a paint that DROVE the icon (cpBatteryIconEndPaint).
bool s_hasBaseline = false;
bool s_lastCharging = false;
// The last skip reason LOGGED for the edge currently pending. A pending edge re-tests on
// every loop iteration; this keeps that from filling the log while still reporting a change
// of reason, and it is cleared whenever the edge is settled.
const char* s_reportedWhy = nullptr;

// BATT2: the COMPOSED claim. cpBatteryIconNotePaint() runs while a frame is being drawn,
// which is before anyone knows whether that frame will be driven (the entry gate holds the
// first drive of a screen; an activity can decline its own frame). So the compose site only
// records what it drew; cpBatteryIconEndPaint() commits it to s_lastCharging iff the paint
// actually drove. Without this split, a held paint consumed the edge and the icon stayed
// stale with nothing in the log to say why.
bool s_hasClaim = false;
bool s_claimCharging = false;

// BATT4 heartbeat counters: what this subsystem has done since boot, so the log
// distinguishes "no edge ever fired" from "edges fired and were skipped".
uint32_t s_hbEdges = 0;
uint32_t s_hbDrives = 0;
uint32_t s_hbSkips = 0;
uint32_t s_hbLastDrawMs = 0;

bool rectPlausible(const GfxRenderer& renderer) {
  if (!s_valid) return false;
  if (s_w < kMinIconSide || s_h < kMinIconSide) return false;
  if (s_w > kMaxIconSide || s_h > kMaxIconSide) return false;
  if (s_x < 0 || s_y < 0) return false;
  if (s_x + s_w + 1 > renderer.getScreenWidth()) return false;
  if (s_y + s_h > renderer.getScreenHeight()) return false;
  return true;
}

}  // namespace

void cpBatteryIconBeginPaint() {
  s_valid = false;
  s_hasClaim = false;
}

void cpBatteryIconNotePaint(const int x, const int y, const int width, const int height) {
  s_x = x;
  s_y = y;
  s_w = width;
  s_h = height;
  s_valid = width > 0 && height > 0;
  // BATT2: record what this frame DRAWS (the glyph reads isCharging() live), but do not
  // claim the glass yet -- cpBatteryIconEndPaint() commits it only if the paint drove.
  if (s_valid) {
    s_hasClaim = true;
    s_claimCharging = powerManager.isCharging();
  }
}

// BATT2: end of an activity paint. `drove` is ActivityManager's own verdict
// (`!held && !declined`), i.e. the same evidence its `[ACT] paint ... drove=N` line prints.
void cpBatteryIconEndPaint(const bool drove) {
  if (!s_hasClaim) return;  // this frame did not draw the icon: nothing to claim
  s_hasClaim = false;
  if (!drove) {
    // Composed but withheld: the glass still shows the PREVIOUS state, so the edge stays
    // pending. This is the case that used to eat the update silently.
    LOG_INF("BATT", "icon claim=%d withheld (drove=0): glass unchanged, edge stays pending",
            s_claimCharging ? 1 : 0);
    return;
  }
  s_hasBaseline = true;
  s_lastCharging = s_claimCharging;
  s_reportedWhy = nullptr;
}

bool cpBatteryIconVisibleOnScreen() { return s_valid; }

CpBatteryIconResult cpBatteryIconPollAndRepaint(GfxRenderer& renderer) {
  // A CACHED read: HalPowerManager refreshes it at most once per BATTERY_POLL_MS
  // (1.5 s). Reading it every loop iteration therefore cannot fight the poll, and a
  // steady state produces exactly zero repaints -- the comparison below is an edge,
  // not a level.
  const bool charging = powerManager.isCharging();

  // BATT4: the observable heartbeat. The operator's debug cable IS the charger, so
  // while the shell is up isCharging() is pinned at 1 and NO edge can ever fire --
  // which made every line in this file unreachable exactly when someone was watching.
  //
  // r106 B3 (R103_GLASS_LOG §F5): this line used to be INFO every 5 s. It is ~180
  // bytes, so it filled the 20-line RAM ring and dominated the card log, and the
  // panel's own `[PANEL] show ... ms=` line for a page turn was evicted within
  // seconds -- a drive-cost measurement became impossible. It is now:
  //   * INFO on the FIRST sample and whenever the visible state changes
  //     (charging / last / vis), so a state change is still findable at INFO, and
  //   * DEBUG otherwise, i.e. only in the ring/file when the debug gate is on.
  // The 5 s cadence is kept for the DEBUG case: "the poll is alive" is still answerable
  // by raising `loglevel debug`, it just no longer costs the ring by default.
  //   charging  live isCharging()
  //   last      the state the GLASS is believed to show (edge compares against this)
  //   base      1 = a baseline exists (0 only on the very first loop after boot)
  //   vis       1 = the last paint recorded an icon rect (this file's "is it on screen")
  //   rect      that rect, in logical coords: x,y WxH where W already includes the nub
  //   promo/clean/gate  the three one-shot guards that make the repaint skip
  //   n/drv/skip        edges seen / drives issued / skips issued since boot
  //   lastms    duration of the last drive: ~0-3 ms means the panel found nothing to
  //             diff (the box redrew identically) -- the "stale rect" case
  {
    static uint32_t hbMs = 0;
    static int hbKey = -1;  // last (charging, lastCharging, visible) triple seen
    const int key = (charging ? 1 : 0) | (s_lastCharging ? 2 : 0) | (s_valid ? 4 : 0);
    const bool changed = key != hbKey;
    const uint32_t nowMs = millis();
    if (changed || hbMs == 0 || nowMs - hbMs >= 5000) {
      hbMs = nowMs;
      hbKey = key;
      if (changed) {
        LOG_INF("BATT",
                "icon hb charging=%d last=%d base=%d vis=%d rect=(%d,%d %dx%d) promo=%d clean=%d gate=%d "
                "n=%lu drv=%lu skip=%lu lastms=%lu%s",
                charging ? 1 : 0, s_lastCharging ? 1 : 0, s_hasBaseline ? 1 : 0, s_valid ? 1 : 0, s_x, s_y,
                s_w + 1, s_h, renderer.hasPromotedRefresh() ? 1 : 0, renderer.forcedCleanArmed() ? 1 : 0,
                renderer.entryGateArmed() ? 1 : 0, static_cast<unsigned long>(s_hbEdges),
                static_cast<unsigned long>(s_hbDrives), static_cast<unsigned long>(s_hbSkips),
                static_cast<unsigned long>(s_hbLastDrawMs), " [state change]");
      } else {
        LOG_DBG("BATT",
                "icon hb charging=%d last=%d base=%d vis=%d rect=(%d,%d %dx%d) promo=%d clean=%d gate=%d "
                "n=%lu drv=%lu skip=%lu lastms=%lu",
                charging ? 1 : 0, s_lastCharging ? 1 : 0, s_hasBaseline ? 1 : 0, s_valid ? 1 : 0, s_x, s_y,
                s_w + 1, s_h, renderer.hasPromotedRefresh() ? 1 : 0, renderer.forcedCleanArmed() ? 1 : 0,
                renderer.entryGateArmed() ? 1 : 0, static_cast<unsigned long>(s_hbEdges),
                static_cast<unsigned long>(s_hbDrives), static_cast<unsigned long>(s_hbSkips),
                static_cast<unsigned long>(s_hbLastDrawMs));
      }
    }
  }

  if (!s_hasBaseline) {
    s_hasBaseline = true;
    s_lastCharging = charging;
    LOG_INF("BATT", "icon baseline state=%d (no repaint)", charging ? 1 : 0);
    return CpBatteryIconResult::Idle;
  }
  if (charging == s_lastCharging) return CpBatteryIconResult::Idle;

  // BATT1R: the edge is NOT spent here any more. It is spent only when the glass is
  // brought up to date -- by a successful repaint below, or by a paint that DROVE the
  // icon (cpBatteryIconEndPaint), or by the "there is no icon on this screen" case,
  // where there is nothing that can go stale and the next screen paints fresh anyway.
  const char* why = nullptr;
  ++s_hbEdges;

  // The decision ledger: one line per event (NOT per loop -- the pending retest below is
  // rate-limited), so "did the request happen, and did the draw happen" is answerable from
  // the log alone.
  LOG_INF("BATT", "icon edge=charge state=%d repaint=requested rect=(%d,%d %dx%d)", charging ? 1 : 0, s_x, s_y,
          s_w + 1, s_h);

  // Two cases are SETTLED here rather than deferred: this path has no rectangle to
  // repaint, and retesting it every loop would never produce one (the rect only changes
  // when some paint draws the glyph). Before BATT4 the edge was settled and nothing at
  // all happened, which is the total hole: main.cpp's whole-activity fallback skipped the
  // reader unconditionally, so a reader screen whose glyph is not recorded had NO
  // producer. It now returns NeedsFullRepaint and main.cpp repaints the activity -- the
  // r97 producer, used only for the screens this one cannot address.
  if (!cpBatteryIconVisibleOnScreen()) {
    s_lastCharging = charging;
    ++s_hbSkips;
    LOG_INF("BATT",
            "icon edge=charge state=%d repaint=fallback reason=no-icon-on-screen (edge settled, full activity "
            "repaint requested)",
            charging ? 1 : 0);
    return CpBatteryIconResult::NeedsFullRepaint;
  }
  if (!rectPlausible(renderer)) {
    s_valid = false;
    s_lastCharging = charging;
    ++s_hbSkips;
    LOG_INF("BATT",
            "icon edge=charge state=%d repaint=fallback reason=bad-rect (edge settled, full activity repaint "
            "requested) rect=(%d,%d %dx%d)",
            charging ? 1 : 0, s_x, s_y, s_w + 1, s_h);
    return CpBatteryIconResult::NeedsFullRepaint;
  }

  if (renderer.hasPromotedRefresh() || renderer.forcedCleanArmed()) {
    // Consuming an armed one-shot here would turn a one-pixel icon update into the
    // very from-white clean / whole-panel GC16 this switch exists to avoid. The
    // wait is short: the screen that armed it paints next and draws the new state,
    // and that paint settles the edge through cpBatteryIconEndPaint().
    why = "a one-shot refresh is armed (promotion or forced clean) -- not consuming it";
  } else if (renderer.entryGateArmed()) {
    why = "entry gate armed: this frame's first drive is being held";
  }

  if (why == nullptr) {
    // Try, never block: a paint in flight means the frame on the glass is being
    // replaced right now, and that paint already reads the new charging state.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      why = "paint in flight";
    } else {
      const uint32_t t0 = millis();
      // 1. Paper back over the glyph's ink. The nub lives at x + w, so the box is
      //    (w + 1) wide; the body is h tall.
      renderer.fillRect(s_x, s_y, s_w + 1, s_h, /*state=*/false);
      // 2. Repaint the icon through the SAME primitives the call site used, so the
      //    pixels are identical to what a full paint would produce -- theme art
      //    (Lyra's solid-when-charging fill, the bolt) included.
      BaseTheme::drawBatteryOutline(renderer, s_x, s_y, s_w, s_h);
      UITheme::getInstance().getTheme().fillBatteryIcon(
          renderer, Rect{s_x, s_y, s_w, s_h}, powerManager.getBatteryPercentage());
      // 3. Drive. FAST_REFRESH is DU, and the ONLY thing that changed in the
      //    framebuffer is the box above, so epd_difference_image_cropped() hands
      //    epd_draw_base() a mask of those rows and those column bytes.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t ms = millis() - t0;
      s_hasBaseline = true;
      s_lastCharging = charging;  // the glass now shows this state
      s_reportedWhy = nullptr;
      ++s_hbDrives;
      s_hbLastDrawMs = ms;
      LOG_INF("BATT", "icon draw state=%d rect=(%d,%d %dx%d) mode=DU ms=%lu", charging ? 1 : 0, s_x, s_y, s_w + 1,
              s_h, static_cast<unsigned long>(ms));
      return CpBatteryIconResult::Drove;
    }
  }

  // Pending when a paint will follow, settled above when one cannot. Either way the edge
  // is not lost: the next loop re-tests it, and any paint that DRAWS + DRIVES the icon
  // settles it. One line per CHANGE of reason, not per loop iteration.
  if (why != s_reportedWhy) {
    s_reportedWhy = why;
    ++s_hbSkips;
    LOG_INF("BATT", "icon edge=charge state=%d repaint=skipped reason=%s (edge stays pending)", charging ? 1 : 0,
            why);
  }
  return CpBatteryIconResult::Pending;
}

#endif  // CP_ICON_PARTIAL_REPAINT
