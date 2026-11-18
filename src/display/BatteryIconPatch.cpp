// BATT5 — CP_ICON_SAFE_PATCH: the battery glyph's own rectangle, driven as a
// diff-masked DU, LICENSED BY A PROVABLE FACT (canvas == glass) instead of by the
// inference the retired r98 path used (a rect was recorded).
//
// Everything that decides whether this may run at all is in
// lib/GfxRenderer/GlassEpoch.h; the rect bookkeeping is on the renderer, next to
// the epochs, so the two cannot drift apart. This file is only the mechanism:
// erase the box, redraw the glyph through the SAME primitives the paint used,
// pin the drive to DU, drive.
//
// WHY THE BLANK FRAME IS IMPOSSIBLE HERE (the failure that retired r98):
//   * the canvas outside the box is, by the licence, the frame the panel is
//     showing, so the panel's own framebuffer-vs-glass diff IS the box;
//   * the drive is pinned with forceBinaryDuOnce() + suppressCompletionDriveOnce()
//     -- the FLP2 pair 58b4d2cc introduced -- so the vendor shim cannot pick
//     `gray8-wipe-GL16(38)` (my_waveform_wipe_gl(3): eight phases that drive EVERY
//     pixel to paper, followed by a repaint of the diff mask ONLY, which is
//     exactly "whole panel white, icon box redrawn" = the corner-only blank
//     screen), and there is no row-9 whole-panel follow-up;
//   * both the epochs and the rect epoch are re-read INSIDE the RenderLock, right
//     before the first pixel is touched.

#include "display/BatteryIconPatch.h"

#include "display/RefreshPolicy.h"

#if CP_ICON_SAFE_PATCH

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace {

// Design-space body is 15-16 x 12 px (ThemeMetrics::batteryWidth/Height, scaled to
// panel pixels by the density table), and the nub adds one column. The bounds are
// deliberately loose: they exist to reject a garbage record, not to re-derive the
// theme's geometry (recording at the draw site is what avoids that).
constexpr int kMinIconSide = 4;
constexpr int kMaxIconSide = 64;

// Edge state. lastCharging is the state the GLASS is believed to show, so it only
// advances when a drive landed or when the edge was settled by an authoritative
// answer (no addressable rect on this screen).
bool s_hasBaseline = false;
bool s_lastCharging = false;
uint32_t s_pendingSinceMs = 0;
bool s_hasReported = false;
cpglass::Refusal s_reportedWhy = cpglass::Refusal::None;

// Counters since boot: tells "no edge ever fired" apart from "edges fired and were
// skipped" from the log alone.
uint32_t s_edges = 0;
uint32_t s_drives = 0;
uint32_t s_skips = 0;
uint32_t s_lastMs = 0;

// How long an owed edge may wait on the epoch fact before the caller is told to
// take the whole-activity path. This is the ONLY escape hatch from a stuck epoch:
// without it a fact that never becomes true would leave the glyph stale for the
// rest of the session with nothing but a log line. 1.5 s is one battery poll, so a
// healthy transition never reaches it.
constexpr uint32_t kPendingGiveUpMs = 1500;

bool rectPlausible(const GfxRenderer& renderer, const GfxRenderer::BatteryIconRect& r) {
  if (!r.valid) return false;
  if (r.w < kMinIconSide || r.h < kMinIconSide) return false;
  if (r.w > kMaxIconSide || r.h > kMaxIconSide) return false;
  if (r.x < 0 || r.y < 0) return false;
  if (r.x + r.w + 1 > renderer.getScreenWidth()) return false;
  if (r.y + r.h > renderer.getScreenHeight()) return false;
  return true;
}

// The licence, assembled from the renderer and this file's rect record.
cpglass::Licence buildLicence(const GfxRenderer& renderer, const bool guardsClear) {
  const auto& r = renderer.batteryIconRect();
  cpglass::Licence lic;
  lic.canvasIsGlass = renderer.framebufferIsGlassFrame();
  lic.hasRect = r.valid;
  lic.rectPlausible = rectPlausible(renderer, r);
  lic.rectEpoch = r.epoch;
  lic.glassEpoch = renderer.framebufferGlassEpoch();
  lic.guardsClear = guardsClear;
  return lic;
}

void logHeartbeat(const GfxRenderer& renderer, const bool charging) {
  static uint32_t hbMs = 0;
  static int hbKey = -1;
  const auto& r = renderer.batteryIconRect();
  const int key = (charging ? 1 : 0) | (s_lastCharging ? 2 : 0) | (r.valid ? 4 : 0);
  const bool changed = key != hbKey;
  const uint32_t nowMs = millis();
  if (!changed && hbMs != 0 && nowMs - hbMs < 5000) return;
  hbMs = nowMs;
  hbKey = key;
  // charging  live isCharging()
  // last      the state the GLASS is believed to show
  // base      1 = a baseline exists
  // vis       1 = a paint recorded a rect
  // canvas/glass  the epochs: equal == the canvas IS the frame on the panel
  // rect      that rect in logical coords, W already including the nub
  // epoch     the compose epoch the rect was COMMITTED at (0 = not committed)
  if (changed) {
    LOG_INF("BATT",
            "icon hb charging=%d last=%d base=%d vis=%d canvas=%lu glass=%lu rect=(%d,%d %dx%d) epoch=%lu "
            "n=%lu drv=%lu skip=%lu lastms=%lu [state change]",
            charging ? 1 : 0, s_lastCharging ? 1 : 0, s_hasBaseline ? 1 : 0, r.valid ? 1 : 0,
            static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()), r.x, r.y, r.w + 1, r.h,
            static_cast<unsigned long>(r.epoch), static_cast<unsigned long>(s_edges),
            static_cast<unsigned long>(s_drives), static_cast<unsigned long>(s_skips),
            static_cast<unsigned long>(s_lastMs));
  } else {
    LOG_DBG("BATT",
            "icon hb charging=%d last=%d base=%d vis=%d canvas=%lu glass=%lu rect=(%d,%d %dx%d) epoch=%lu "
            "n=%lu drv=%lu skip=%lu lastms=%lu",
            charging ? 1 : 0, s_lastCharging ? 1 : 0, s_hasBaseline ? 1 : 0, r.valid ? 1 : 0,
            static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()), r.x, r.y, r.w + 1, r.h,
            static_cast<unsigned long>(r.epoch), static_cast<unsigned long>(s_edges),
            static_cast<unsigned long>(s_drives), static_cast<unsigned long>(s_skips),
            static_cast<unsigned long>(s_lastMs));
  }
}

// The refusal is owed, not settled: retry next loop iteration, unless the wait has
// outlived kPendingGiveUpMs -- then hand the edge to the whole-activity path.
CpBatteryIconResult pendingOrGiveUp(const GfxRenderer& renderer, const bool charging, const cpglass::Refusal why) {
  if (s_pendingSinceMs == 0) s_pendingSinceMs = millis();
  if (millis() - s_pendingSinceMs >= kPendingGiveUpMs) {
    s_pendingSinceMs = 0;
    s_lastCharging = charging;  // settled: the caller will repaint the screen
    ++s_skips;
    LOG_INF("BATT",
            "icon edge=charge state=%d repaint=fallback reason=%s-held-for-%lums canvas=%lu glass=%lu "
            "(edge settled, full activity repaint requested)",
            charging ? 1 : 0, cpglass::refusalName(why), static_cast<unsigned long>(kPendingGiveUpMs),
            static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()));
    return CpBatteryIconResult::NeedsFullRepaint;
  }
  if (!s_hasReported || s_reportedWhy != why) {
    s_hasReported = true;
    s_reportedWhy = why;
    ++s_skips;
    LOG_INF("BATT",
            "icon edge=charge state=%d repaint=skipped reason=%s canvas=%lu glass=%lu rectEpoch=%lu "
            "(edge stays pending)",
            charging ? 1 : 0, cpglass::refusalName(why),
            static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()),
            static_cast<unsigned long>(renderer.batteryIconRect().epoch));
  }
  return CpBatteryIconResult::Pending;
}

}  // namespace

CpBatteryIconResult cpBatteryIconPatchOnEdge(GfxRenderer& renderer) {
  // A CACHED read: HalPowerManager refreshes it at most once per BATTERY_POLL_MS
  // (1.5 s), so this can neither fight the poll nor repaint on a steady state.
  const bool charging = powerManager.isCharging();

  logHeartbeat(renderer, charging);

  if (!s_hasBaseline) {
    s_hasBaseline = true;
    s_lastCharging = charging;
    LOG_INF("BATT", "icon baseline state=%d (no repaint)", charging ? 1 : 0);
    return CpBatteryIconResult::Idle;
  }
  if (charging == s_lastCharging) {
    s_pendingSinceMs = 0;
    s_hasReported = false;
    return CpBatteryIconResult::Idle;
  }

  ++s_edges;
  const bool guardsClear =
      !renderer.hasPromotedRefresh() && !renderer.forcedCleanArmed() && !renderer.entryGateArmed();
  const cpglass::Licence lic = buildLicence(renderer, guardsClear);
  const cpglass::Refusal why = cpglass::refusal(lic);

  LOG_INF("BATT",
          "icon edge=charge state=%d decision=%s canvas=%lu glass=%lu rect=(%d,%d %dx%d) rectEpoch=%lu",
          charging ? 1 : 0, cpglass::refusalName(why),
          static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
          static_cast<unsigned long>(renderer.framebufferGlassEpoch()), renderer.batteryIconRect().x,
          renderer.batteryIconRect().y, renderer.batteryIconRect().w + 1, renderer.batteryIconRect().h,
          static_cast<unsigned long>(renderer.batteryIconRect().epoch));

  // No addressable rect: nothing this subsystem can draw is stale, and only a
  // whole-activity repaint can move the glyph (a screen that draws its battery
  // inside the FreeInkUI header, for instance). Settle and say so.
  if (cpglass::needsWholeActivityRepaint(why)) {
    s_lastCharging = charging;
    s_pendingSinceMs = 0;
    ++s_skips;
    LOG_INF("BATT",
            "icon edge=charge state=%d repaint=fallback reason=%s (edge settled, full activity repaint requested)",
            charging ? 1 : 0, cpglass::refusalName(why));
    return CpBatteryIconResult::NeedsFullRepaint;
  }

  if (!cpglass::licensed(lic)) return pendingOrGiveUp(renderer, charging, why);

  // Try, never block: a paint in flight means the frame on the glass is being
  // replaced right now, and that paint reads the new charging state itself.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return pendingOrGiveUp(renderer, charging, cpglass::Refusal::GuardArmed);

  // RE-READ the licence inside the lock, immediately before touching a pixel: a
  // paint that started between the decision above and this line would have moved
  // the canvas off the glass, and the whole safety argument depends on this being
  // checked here and not only above.
  const cpglass::Licence locked = buildLicence(renderer, /*guardsClear=*/true);
  if (!cpglass::licensed(locked)) return pendingOrGiveUp(renderer, charging, cpglass::refusal(locked));

  // A COPY: drawBatteryOutline() below re-records the rect, which rewrites the
  // renderer's member (geometry identical, epoch back to "uncommitted"). Holding a
  // reference would alias that write, and the epoch must be re-committed after the
  // drive anyway or the NEXT edge would be refused for a rect that IS on the glass.
  const GfxRenderer::BatteryIconRect r = renderer.batteryIconRect();
  const uint32_t t0 = millis();
  // 1. Paper back over the glyph's ink. The nub lives at x + w, so the box is
  //    (w + 1) wide; the body is h tall.
  renderer.fillRect(r.x, r.y, r.w + 1, r.h, /*state=*/false);
  // 2. Redraw through the SAME primitives the paint used, so the pixels are
  //    identical to what a full paint would produce -- theme art (Lyra's
  //    solid-when-charging fill, the bolt) included.
  BaseTheme::drawBatteryOutline(renderer, r.x, r.y, r.w, r.h);
  UITheme::getInstance().getTheme().fillBatteryIcon(renderer, Rect{r.x, r.y, r.w, r.h},
                                                    powerManager.getBatteryPercentage());
  // 3. Pin the drive: diff-masked DU, no grey-wipe lane, no row-9 follow-up. Only
  //    the box above differs from the glass, so the panel's own diff mask is the
  //    box and nothing else.
  renderer.forceBinaryDuOnce();
  renderer.suppressCompletionDriveOnce();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t ms = millis() - t0;

  // 4. VERIFY, from the renderer's own bookkeeping: the drive was accepted only if
  //    the compose epoch it just drove is now the glass epoch. A failed drive bumps
  //    the compose epoch instead, so this is false and nothing is settled.
  if (!renderer.framebufferIsGlassFrame()) {
    ++s_skips;
    s_pendingSinceMs = 0;
    s_lastCharging = charging;  // settled: the caller repaints the screen properly
    LOG_ERR("BATT",
            "icon patch drive did NOT land canvas=%lu glass=%lu (edge handed to the full activity repaint)",
            static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()));
    return CpBatteryIconResult::NeedsFullRepaint;
  }

  s_lastCharging = charging;  // the glass now shows this state
  s_pendingSinceMs = 0;
  s_hasReported = false;
  ++s_drives;
  s_lastMs = ms;
  // The outline above re-recorded the rect with epoch 0 ("uncommitted"); this paint
  // DID reach the glass, so stamp it. Without this the next plug/unplug would be
  // refused on a rect whose ink is on the panel.
  renderer.commitBatteryIconRect(/*drove=*/true);
  // BATT6: the percentage label drawn by the same paint is still on the glass -- this
  // drive touched the glyph's box only -- so its record has to move to the epoch the
  // glass now holds as well. Otherwise the sibling producer would sit on a box whose
  // rectEpoch the glass has passed and wait for a full repaint that may not come for
  // minutes.
  renderer.commitBatteryPercentRect(/*drove=*/true);
  LOG_INF("BATT", "icon patch state=%d rect=(%d,%d %dx%d) mode=DU ms=%lu canvas=%lu glass=%lu",
          charging ? 1 : 0, r.x, r.y, r.w + 1, r.h, static_cast<unsigned long>(ms),
          static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
          static_cast<unsigned long>(renderer.framebufferGlassEpoch()));
  return CpBatteryIconResult::Drove;
}

// BATT6: another producer (the percentage's own box repaint) redrew this glyph
// with the LIVE charging state and drove it. Settle this subsystem's edge against
// that evidence instead of spending a second drive on it. Nothing else is touched:
// s_edges and s_drives count this file's own work, and s_lastMs stays the duration
// of the last drive THIS file made.
void cpBatteryIconPatchNoteExternalDraw(const bool charging) {
  s_hasBaseline = true;
  s_lastCharging = charging;
  s_pendingSinceMs = 0;
  s_hasReported = false;
  LOG_INF("BATT", "icon settled by the soc repaint state=%d (no second drive)", charging ? 1 : 0);
}

#endif  // CP_ICON_SAFE_PATCH
