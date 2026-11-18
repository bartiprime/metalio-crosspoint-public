// BATT6 (r165.2) — the percentage NUMBER's change producer. See
// display/BatteryPercentPatch.h for what this is for and which surfaces it covers,
// and display/BatteryPercentChange.h for the decision it makes (host-tested there).
//
// r165.2 REWRITE. The r165.1 producer asked for a whole-activity repaint on every
// pass whenever the record's value could not be credited, and that value can never
// be credited on a surface with no addressable box (the FreeInkUI header band): a
// repaint of a screen whose pixels did not change does not drive, so the epoch
// stayed 0 and the matching value was never accepted. One session on glass:
// twelve `soc paint=0 90%->90% repaint=fallback` and not one `paint=1`; one of
// those repaints consumed the armed return-to-Home clean (the ghosting).
//
// Three changes make that impossible:
//
//   1. The DECISION credits an unaddressable record's VALUE (BatteryPercentChange.h:
//      "a skipped drive means the frame already on the glass was composed with that
//      value"), so a steady value is worth nothing at all.
//   2. The REQUEST MEMORY is keyed on (surface, value) and written at ISSUE, never
//      on a successful paint, so a surface that cannot be patched asks at most once
//      per distinct value, forever.
//   3. A whole-activity repaint is never requested from here. It is ARMED (a flag)
//      and consumed once by the main loop at its own boundary, and never while a
//      clean is armed or a transition is in flight.

#include "display/BatteryPercentPatch.h"

#include "display/RefreshPolicy.h"

#if CP_BATT_PERCENT_PATCH

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "display/BatteryIconPatch.h"
#include "display/BatteryPercentChange.h"

namespace {

// Bounds, deliberately loose: they exist to reject a garbage record (an
// uninitialised or absurd box), not to re-derive any theme's geometry --
// recording at the draw site is what avoids that.
constexpr int kMinLineHeight = 4;
constexpr int kMaxLineHeight = 64;
constexpr int kMaxTextWidth = 200;

using PctRect = GfxRenderer::BatteryPercentRect;

// Counters since boot, so "no change ever seen" is distinguishable from "changes
// seen and refused" from the log alone.
uint32_t s_changes = 0;
uint32_t s_drives = 0;
uint32_t s_skips = 0;
uint32_t s_lastMs = 0;
bool s_hasReported = false;
cpglass::Refusal s_reportedWhy = cpglass::Refusal::None;

// One outstanding whole-activity request at a time, per (surface, value), with a
// 10 s per-surface floor (see battpct::Requests). Without the value key an
// unaddressable surface would ask on every pass; without the floor an adversarial
// alternation would ask on every step.
battpct::Requests s_requests;

// The ONE deferred request. Armed here, consumed by
// cpBatteryPercentTakeDeferred() at the main loop's own boundary: asking for a
// whole-activity repaint from inside this call would re-enter the activity manager.
bool s_deferredArmed = false;

bool boxPlausible(const GfxRenderer& renderer, const PctRect& r) {
  if (!r.valid) return false;
  if (r.fontId < 0) return false;
  if (r.w < 0 || r.w > kMaxTextWidth) return false;
  if (r.h < kMinLineHeight || r.h > kMaxLineHeight) return false;
  if (r.x < 0 || r.y < 0) return false;
  if (r.anchorX < 0 || r.anchorX > renderer.getScreenWidth()) return false;
  // The box's right edge: for a right-aligned label the anchor IS that edge.
  const int right = r.align == PctRect::Right ? r.anchorX : r.x + r.w;
  if (right > renderer.getScreenWidth()) return false;
  if (r.y + r.h > renderer.getScreenHeight()) return false;
  return true;
}

// The surface a request would be FOR: the geometry this path could repaint, or 0
// for "recorded, but no box this path can draw" (the FreeInkUI header band, a
// garbage box, a label whose width changed). Two different unaddressable screens
// share key 0, which can only ever SUPPRESS a redundant request -- never cause one.
uint32_t surfaceOf(const PctRect& r, const bool plausible) {
  if (!plausible) return 0;
  uint32_t h = 2166136261u;
  const int16_t fields[5] = {r.x, r.y, r.w, r.h, r.fontId};
  for (const int16_t f : fields) {
    h ^= static_cast<uint16_t>(f);
    h *= 16777619u;
  }
  return h | 1u;  // never 0
}

cpglass::Licence buildLicence(const GfxRenderer& renderer, const bool guardsClear) {
  const auto& p = renderer.batteryPercentRect();
  cpglass::Licence lic;
  lic.canvasIsGlass = renderer.framebufferIsGlassFrame();
  lic.hasRect = p.valid;
  lic.rectPlausible = boxPlausible(renderer, p);
  lic.rectEpoch = p.epoch;
  lic.glassEpoch = renderer.framebufferGlassEpoch();
  lic.guardsClear = guardsClear;
  return lic;
}

void logPending(const uint16_t soc, const int16_t shown, const cpglass::Refusal why, const GfxRenderer& renderer) {
  if (s_hasReported && s_reportedWhy == why) return;
  s_hasReported = true;
  s_reportedWhy = why;
  ++s_skips;
  LOG_INF("BATT",
          "soc paint=0 %d%%->%d%% reason=%s canvas=%lu glass=%lu (owed, retried next pass)",
          static_cast<int>(shown), static_cast<int>(soc), cpglass::refusalName(why),
          static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
          static_cast<unsigned long>(renderer.framebufferGlassEpoch()));
}

}  // namespace

CpBatteryIconResult cpBatteryPercentPatchOnChange(GfxRenderer& renderer) {
  // A CACHED read: HalPowerManager refreshes it at most once per BATTERY_POLL_MS
  // (1.5 s) and the main loop keeps that cache warm (powerManager.pollBattery()),
  // so this can neither fight the poll nor repaint on a steady value -- the
  // comparison below is an edge against what the GLASS was painted with.
  const uint16_t soc = powerManager.getBatteryPercentage();
  const PctRect& rec = renderer.batteryPercentRect();
  const int16_t shown = rec.value;

  // The text this repaint would print, measured in the face the box was recorded in.
  // Both the width guard below and the drive itself use this one measurement.
  char text[8];
  snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(soc));
  const int newTextW = rec.fontId >= 0 ? renderer.getTextWidth(rec.fontId, text) : -1;

  // A clean that is armed but not yet consumed, and a transition whose entry gate
  // is up, are the two states in which a request races the paint that is already
  // going to draw the live value. On the reported session one such request consumed
  // the return-to-Home clean; they are named here so the decision can refuse them
  // by their own name (and so the host test can set them directly).
  const bool cleanArmed = renderer.forcedCleanArmed();
  const bool transition = renderer.entryGateArmed();
  const bool guardsClear = !renderer.hasPromotedRefresh() && !cleanArmed && !transition;
  const cpglass::Licence lic = buildLicence(renderer, guardsClear);

  battpct::View v;
  v.hasRect = lic.hasRect;
  v.rectPlausible = lic.rectPlausible;
  v.sameWidth = newTextW < 0 || newTextW == rec.w;
  v.shownValue = shown;
  v.rectEpoch = lic.rectEpoch;
  v.glassEpoch = lic.glassEpoch;
  v.canvasIsGlass = lic.canvasIsGlass;
  v.guardsClear = lic.guardsClear;
  v.cleanArmed = cleanArmed;
  v.transition = transition;

  switch (battpct::need(v, soc)) {
    case battpct::Need::None:
      // No number on this screen, or the glass already reads this value: zero work.
      // NOTE the absence of any reset of s_requests here: "nothing owed" is not
      // evidence that a request was served, and treating it as one is what let the
      // r165.1 producer ask again after every repaint.
      s_hasReported = false;
      return CpBatteryIconResult::Idle;

    case battpct::Need::Deferred:
      // Recorded, but not this path's to draw: no box at all (the FreeInkUI header),
      // a box that cannot be on this panel, or a label whose WIDTH changes and so
      // needs the screen re-laid out. ONE request per (surface, value), at least
      // kMinIntervalMs apart (the decision has already refused a clean or a
      // transition as Pending, so nothing here races the paint in flight).
      if (!s_requests.take(surfaceOf(rec, lic.rectPlausible), soc, millis())) return CpBatteryIconResult::Idle;
      ++s_skips;
      LOG_INF("BATT",
              "soc paint=0 %d%%->%d%% repaint=deferred reason=%s "
              "(one whole-activity repaint, at the loop boundary)",
              static_cast<int>(shown), static_cast<int>(soc),
              lic.rectPlausible ? "label-width-change" : "unaddressable");
      // Armed, not requested: main.cpp consumes this once per loop pass, outside
      // any paint, so nothing here can re-enter the activity manager.
      s_deferredArmed = true;
      return CpBatteryIconResult::Idle;

    case battpct::Need::Pending:
      // Owed, but a guard, a clean or an epoch holds it. This costs ZERO commands
      // and is never escalated: the paint that lifts the guard draws the live
      // value, and only a value that genuinely differs may ask for a repaint.
      logPending(soc, shown, cpglass::refusal(lic), renderer);
      return CpBatteryIconResult::Pending;

    case battpct::Need::Patch:
      break;
  }

  // Try, never block: a paint in flight is composing a frame that draws the live
  // percentage itself.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) {
    logPending(soc, shown, cpglass::Refusal::GuardArmed, renderer);
    return CpBatteryIconResult::Pending;
  }

  // RE-READ the licence inside the lock, immediately before the first pixel: a
  // paint that started between the decision above and this line would have moved
  // the canvas off the glass, and the whole safety argument depends on this check
  // being here and not only above.
  const cpglass::Licence locked = buildLicence(renderer, /*guardsClear=*/true);
  if (!cpglass::licensed(locked)) {
    logPending(soc, shown, cpglass::refusal(locked), renderer);
    return CpBatteryIconResult::Pending;
  }

  // A COPY: the draws below re-record both boxes on the renderer, and the record we
  // are about to stamp must carry the geometry this repaint actually produced.
  const PctRect box = renderer.batteryPercentRect();
  const GfxRenderer::BatteryIconRect icon = renderer.batteryIconRect();
  // The glyph may be repainted in the same drive only when its box belongs to the
  // SAME frame the number's box does -- then both are on the glass, and both are
  // erased and redrawn from the live value. Otherwise the label alone is repainted
  // and the next glyph edge/paint brings the fill along.
  const bool redrawIcon = icon.valid && icon.epoch != 0 && icon.epoch == box.epoch && icon.w > 0 && icon.h > 0;

  const uint32_t t0 = millis();
  // RE-MEASURE inside the lock, in the face the box that is ON THE GLASS was drawn
  // in. A paint that landed between the decision above and this line could have
  // re-laid the cluster out, and then this box is the wrong shape for the text: the
  // next pass sees the new record and takes the re-layout path.
  const int textW = renderer.getTextWidth(box.fontId, text);
  if (textW <= 0 || textW > kMaxTextWidth) {
    // Nothing drawable (a font that cannot render the digits): do not drive a box
    // that would come out blank.
    return CpBatteryIconResult::Idle;
  }
  if (textW != box.w) {
    return CpBatteryIconResult::Idle;
  }

  // 1. Paper over BOTH boxes. The erase spans the wider of the old and the new
  //    text, so 100 % -> 95 % leaves no trailing "%" of the old number behind, and
  //    it is anchored the way the label is (a right-aligned number shrinks toward
  //    its right edge and must be erased from there).
  const int eraseW = std::max<int>(box.w, textW);
  const int eraseX = box.align == PctRect::Right ? box.anchorX - eraseW : box.anchorX;
  renderer.fillRect(std::max(0, eraseX), box.y, eraseW, box.h, /*state=*/false);
  if (redrawIcon) renderer.fillRect(icon.x, icon.y, icon.w + 1, icon.h, /*state=*/false);

  // 2. Redraw through the SAME primitives the paint used, so these pixels are
  //    identical to what a full paint of this screen would produce: the theme's
  //    glyph art (outline, fill level, bolt) and the number in the recorded face.
  if (redrawIcon) {
    BaseTheme::drawBatteryOutline(renderer, icon.x, icon.y, icon.w, icon.h);
    UITheme::getInstance().getTheme().fillBatteryIcon(renderer, Rect{icon.x, icon.y, icon.w, icon.h}, soc);
  }
  const int penX = box.align == PctRect::Right ? box.anchorX - textW : box.anchorX;
  renderer.drawText(box.fontId, penX, box.y, text);

  // 3. Pin the drive: diff-masked DU, no grey-wipe lane, no row-9 follow-up. Only
  //    the two boxes above differ from the glass, so the panel's own diff mask is
  //    those boxes and nothing else -- never a whole-panel clean for a percentage.
  renderer.forceBinaryDuOnce();
  renderer.suppressCompletionDriveOnce();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t ms = millis() - t0;

  // 4. VERIFY from the renderer's own bookkeeping: the drive was accepted only if
  //    the compose epoch it just drove is now the glass epoch. A patch that did not
  //    land does NOT ask for a repaint (that is the r165.1 shape); it waits, and the
  //    next real paint of this screen settles it.
  if (!renderer.framebufferIsGlassFrame()) {
    ++s_skips;
    s_hasReported = false;
    LOG_ERR("BATT",
            "soc patch drive did NOT land %d%% canvas=%lu glass=%lu (the next paint of this screen settles it)",
            static_cast<int>(soc), static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
            static_cast<unsigned long>(renderer.framebufferGlassEpoch()));
    return CpBatteryIconResult::Pending;
  }

  // 5. Stamp BOTH records with the epoch the glass now holds. The rest of the
  //    canvas was already the glass's frame (that is what licensed this), so both
  //    boxes are still on the panel -- and the sibling producers must not be left
  //    waiting for a paint that may not come for minutes.
  PctRect stamped = box;
  stamped.w = static_cast<int16_t>(textW);
  stamped.value = static_cast<int16_t>(soc);
  renderer.noteBatteryPercentRect(stamped);
  renderer.commitBatteryPercentRect(/*drove=*/true);
  if (redrawIcon) {
    renderer.commitBatteryIconRect(/*drove=*/true);
    // Tell the glyph's own producer that the state it tracks is now on the glass:
    // this drive redrew the glyph with the LIVE charging state.
    cpBatteryIconPatchNoteExternalDraw(powerManager.isCharging());
  }

  s_hasReported = false;
  ++s_changes;
  ++s_drives;
  s_lastMs = ms;
  LOG_INF("BATT",
          "soc paint=1 %d%%->%d%% rect=(%d,%d %dx%d) icon=%d mode=DU ms=%lu canvas=%lu glass=%lu "
          "n=%lu drv=%lu skip=%lu",
          static_cast<int>(shown), static_cast<int>(soc), box.x, box.y, box.w, box.h, redrawIcon ? 1 : 0,
          static_cast<unsigned long>(ms), static_cast<unsigned long>(renderer.framebufferComposeEpoch()),
          static_cast<unsigned long>(renderer.framebufferGlassEpoch()), static_cast<unsigned long>(s_changes),
          static_cast<unsigned long>(s_drives), static_cast<unsigned long>(s_skips));
  return CpBatteryIconResult::Drove;
}

bool cpBatteryPercentTakeDeferred() {
  if (!s_deferredArmed) return false;
  s_deferredArmed = false;
  return true;
}

#endif  // CP_BATT_PERCENT_PATCH
