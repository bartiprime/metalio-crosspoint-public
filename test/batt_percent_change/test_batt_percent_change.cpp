// BATT6 (r165.2) — the percentage change decision, on the host.
//
// Everything below is the REAL header the firmware compiles
// (src/display/BatteryPercentChange.h), driven through a model of the renderer's
// record and of the "glass". The properties are asserted as COUNTS:
//
//   * a steady percentage produces ZERO repaint requests, however many passes run;
//   * a surface this path can NEVER address (the FreeInkUI header band, whose
//     whole-activity repaint of an unchanged screen does not drive) asks for
//     EXACTLY ONE repaint per distinct value, then nothing -- forever;
//   * no request while a clean is armed or a transition is in flight;
//   * adversarial alternation is bounded by the 10 s per-surface rate limit;
//   * one change on an addressable box produces exactly one in-place patch;
//   * a screen that draws no number costs nothing at all.
//
// The FIRST test (testUnaddressableAsksOnce) is the r165.1 failure reproduced:
// against the pre-r165.2 header it reported one request per paint, forever. See
// run.sh's "failing first" note and _scratch/r1652/old-header-fail.txt.
//
// Built by run.sh; not part of test/CMakeLists.txt, because it compiles one header
// against nothing else (same standalone shape as test/battstats_cycle_reset).
#include <cstdint>
#include <cstdio>

#include "display/BatteryPercentChange.h"

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(condition)                                               \
  do {                                                                 \
    ++checksRun;                                                       \
    if (!(condition)) {                                                \
      ++checksFailed;                                                  \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                  \
  } while (0)

void section(const char* name) { std::printf("-- %s\n", name); }

// A model of the renderer's batteryPercentRect() + glassEpochs, plus the producer's
// counters -- i.e. exactly the inputs of battpct::need() and of the one piece of
// state the producer keeps.
struct Model {
  battpct::View view;
  battpct::Requests requests;
  uint16_t soc = 100;
  uint32_t nowMs = 0;
  uint32_t patches = 0;      // in-place drives
  uint32_t repaints = 0;     // whole-activity repaints REQUESTED
  int16_t shown = 0;         // what the "glass" reads

  // How the next whole-activity repaint behaves on this surface.
  bool repaintDrives = true;      // a paint whose pixels differ reaches the panel
  bool paintResetsRecord = true;  // ActivityManager resets the record at the top

  // The producer's surface identity: a stable hash of the record it addresses, or
  // 0 for "recorded but no box this path can draw".
  uint32_t surface() const {
    if (!view.hasRect) return 0;
    if (!view.rectPlausible) return 0;
    return 0x9E3779B9u ^ (static_cast<uint32_t>(view.shownValue) << 8);
  }

  // One producer pass, the same shape as cpBatteryPercentPatchOnChange().
  void pass() {
    const battpct::Need need = battpct::need(view, soc);
    switch (need) {
      case battpct::Need::None:
      case battpct::Need::Pending:
        break;
      case battpct::Need::Deferred:
        if (requests.take(surface(), soc, nowMs)) {
          ++repaints;
          paint();  // main.cpp: one whole-activity repaint for this request
        }
        break;
      case battpct::Need::Patch:
        ++patches;
        view.shownValue = static_cast<int16_t>(soc);
        view.rectEpoch = view.glassEpoch + 1;
        view.glassEpoch = view.rectEpoch;
        shown = static_cast<int16_t>(soc);
        break;
    }
  }

  // ActivityManager's paint: reset the record, compose (the activity draws the
  // gauge's LIVE value), then commit with the paint's own `drove` verdict. The
  // producer runs on the main task while this is happening, which is why the
  // reset window matters.
  void paint() {
    if (paintResetsRecord) {
      view.hasRect = false;
      view.rectEpoch = 0;
    }
    // The producer can pass during the paint's own composition window.
    pass();
    // The activity painted the live value into the record.
    view.hasRect = true;
    view.shownValue = static_cast<int16_t>(soc);
    if (repaintDrives) {
      view.rectEpoch = view.glassEpoch + 1;
      view.glassEpoch = view.rectEpoch;
    } else {
      // Skipped drive: the frame the panel is already holding is this one.
      view.rectEpoch = 0;
    }
    // Either way the GLASS now reads this value -- driven, or skipped because the
    // frame already on the glass was composed with the same number.
    shown = static_cast<int16_t>(soc);
  }

  // A tick of the main loop: advance the clock, run one pass.
  void tick(const uint32_t ms = 100) {
    nowMs += ms;
    pass();
  }
};

// An addressable number that is on the glass.
Model live(const uint16_t soc) {
  Model m;
  m.soc = soc;
  m.view.hasRect = true;
  m.view.rectPlausible = true;
  m.view.canvasIsGlass = true;
  m.view.guardsClear = true;
  m.view.shownValue = static_cast<int16_t>(soc);
  m.view.glassEpoch = 7;
  m.view.rectEpoch = 7;
  m.shown = static_cast<int16_t>(soc);
  return m;
}

// The FreeInkUI header band: the number is inside the SDK's battery component, so
// the app has a VALUE and no box -- and a repaint of an unchanged screen does not
// drive, so the epoch stays 0 however many times the activity is repainted.
// `shown` is the value the band's last paint COMPOSED, which is what goes stale.
Model headerBand(const uint16_t soc, const uint16_t shown) {
  Model m;
  m.soc = soc;
  m.view.hasRect = true;
  m.view.rectPlausible = false;
  m.view.canvasIsGlass = true;
  m.view.guardsClear = true;
  m.view.shownValue = static_cast<int16_t>(shown);
  m.view.rectEpoch = 0;
  m.view.glassEpoch = 100;
  m.repaintDrives = false;
  m.shown = static_cast<int16_t>(shown);
  return m;
}

// ---------------------------------------------------------------------------
// THE r165.1 FAILURE, REPRODUCED. On the device the producer logged
//   `soc paint=0 90%->90% repaint=fallback reason=unaddressable` twelve times in
// one session and never once `paint=1`: the value matched the gauge on every
// pass, but the decision demanded an epoch that this surface can never produce,
// and the record reset at the top of each repaint cleared the producer's memory
// through the "nothing owed" branch. One request per paint, forever.
// ---------------------------------------------------------------------------
void testUnaddressableAsksOnce() {
  section("unaddressable surface + steady value: exactly ONE request in 10 000 passes");
  Model m = headerBand(90, 91);  // the band composed 91, the gauge has settled to 90
  for (int i = 0; i < 10000; ++i) m.tick();
  std::printf("   measured: %u requests in 10000 passes\n", m.repaints);
  CHECK(m.repaints == 1);  // r165.1: one per repaint, without end
  CHECK(m.patches == 0);
  CHECK(m.shown == 90);
}

void testUnaddressableOnePerValue() {
  section("unaddressable surface: one request per distinct value, never two");
  Model m = headerBand(90, 91);  // the band composed 91, the gauge has settled to 90
  for (int i = 0; i < 200; ++i) m.tick();
  CHECK(m.repaints == 1);
  // The pack drops a percent 30 s later (the enterprise's own step floor).
  m.soc = 89;
  for (int i = 0; i < 200; ++i) m.tick();
  CHECK(m.repaints == 2);
  // ...and the value returns to one it has already asked for: still bounded by
  // the rate limit and by the value memory, never by a paint outcome.
  m.soc = 90;
  for (int i = 0; i < 200; ++i) m.tick();
  CHECK(m.repaints <= 3);
}

void testNoRequestWhileCleanArmed() {
  section("never request while a clean is armed or a transition is in flight");
  Model m = live(90);
  m.view.cleanArmed = true;
  m.soc = 89;
  for (int i = 0; i < 500; ++i) m.tick();
  CHECK(m.repaints == 0);
  CHECK(m.patches == 0);
  CHECK(m.shown == 90);  // the armed clean's own paint draws the live value
  // Tested directly, on the decision:
  battpct::View v = m.view;
  v.cleanArmed = true;
  CHECK(battpct::need(v, 80) == battpct::Need::Pending);
  v.cleanArmed = false;
  v.transition = true;
  CHECK(battpct::need(v, 80) == battpct::Need::Pending);
  v.transition = false;
  CHECK(battpct::need(v, 80) == battpct::Need::Patch);
}

void testRateLimitUnderAlternation() {
  section("adversarial 90<->91 alternation is bounded by the 10 s rate limit");
  Model m = headerBand(90, 91);  // the band composed 91, the gauge has settled to 90
  uint16_t v = 90;
  for (int i = 0; i < 1000; ++i) {  // 100 s of loop passes at 100 ms
    v = (v == 90) ? 91 : 90;
    m.soc = v;
    m.tick(100);
  }
  std::printf("   measured: %u requests over 100 s of alternation\n", m.repaints);
  // 100 s at one request per 10 s => at most 11, however many values are offered.
  CHECK(m.repaints <= 11);
  CHECK(m.repaints >= 1);
}

// D3 (r165.3). THE DEFECT: `Requests::take()` dropped `timeSet_` whenever the surface changed, so
// two surfaces alternating every pass each measured their interval from zero and the 10 s backstop
// degenerated to one request per pass -- exactly the rate this class exists to bound. The interval
// is now device-wide; only the VALUE memory is per-surface.
void testRateLimitIsGlobalAcrossSurfaces() {
  section("the 10 s backstop is device-wide: alternating surfaces cannot reset it");
  battpct::Requests r;
  int took = 0;
  for (uint32_t t = 5; t <= 100000; t += 5) {  // 100 s at a 5 ms pass
    const bool odd = ((t / 5) & 1u) != 0;
    if (r.take(odd ? 0xAAAAu : 0xBBBBu, odd ? 91 : 90, t)) ++took;
  }
  std::printf("   measured: %d requests over 100 s of 5 ms surface alternation\n", took);
  // floor(100 s / 10 s) + 1 = 11 at most; one per pass would be 20 000.
  CHECK(took <= 11);
  CHECK(took >= 10);
}

void testValueMemoryIsPerSurface() {
  section("the value memory is still per-surface (only the interval is global)");
  battpct::Requests r;
  CHECK(r.take(1, 90, 0));
  CHECK(!r.take(1, 90, 30000));  // same surface, same value: nothing new to ask
  CHECK(r.take(2, 90, 30000));   // a different surface owes its own record
  CHECK(!r.take(2, 90, 35000));  // ...and remembers it
  CHECK(!r.take(3, 91, 39999));  // new surface + new value, 1 ms short of the global floor
  CHECK(r.take(3, 91, 40000));   // floor elapsed: one request
  CHECK(r.issued() == 3);
}

void testSteadyValueIsFree() {
  section("a steady percentage on an addressable box produces zero repaints");
  Model m = live(90);
  for (int i = 0; i < 2000; ++i) m.tick();
  CHECK(m.patches == 0);
  CHECK(m.repaints == 0);
  CHECK(m.shown == 90);
}

void testOneChangeOnePatch() {
  section("one change produces exactly one in-place patch");
  Model m = live(100);
  m.soc = 95;
  m.pass();
  CHECK(m.patches == 1);
  CHECK(m.repaints == 0);
  for (int i = 0; i < 2000; ++i) m.tick();
  CHECK(m.patches == 1);
  CHECK(m.shown == 95);
}

void testBurstCollapses() {
  section("a burst of changes while a guard is armed collapses to one drive");
  Model m = live(100);
  m.view.guardsClear = false;  // a one-shot is armed: nothing may be driven
  int pending = 0;
  for (uint16_t soc = 99; soc >= 60; --soc) {  // 40 changes, 1 % apart
    m.soc = soc;
    for (int i = 0; i < 5; ++i) {  // ...each retried a few times
      m.tick(10);
      if (battpct::need(m.view, m.soc) == battpct::Need::Pending) ++pending;
    }
  }
  CHECK(m.patches == 0);            // blocked: no drive at all
  CHECK(pending > 0);               // ...and it kept asking
  CHECK(m.repaints == 0);           // no whole-activity repaint for a box it can draw
  CHECK(m.view.shownValue == 100);  // the glass still reads the old value

  // The guard clears: ONE drive, with the FINAL value -- not forty queued ones.
  m.view.guardsClear = true;
  m.pass();
  CHECK(m.patches == 1);
  CHECK(m.shown == 60);
  for (int i = 0; i < 500; ++i) m.tick();
  CHECK(m.patches == 1);
}

void testNoNumberOnScreenIsFree() {
  section("a screen that draws no number costs nothing");
  Model m;  // no record at all: a full-screen image, the sleep chip, number hidden
  m.view.hasRect = false;
  m.view.canvasIsGlass = true;
  m.view.guardsClear = true;
  for (uint16_t soc = 100; soc > 20; --soc) {
    m.soc = soc;
    for (int i = 0; i < 10; ++i) m.tick(10);
  }
  CHECK(m.patches == 0);
  CHECK(m.repaints == 0);
}

void testHeldPaintIsNotAGlassClaim() {
  section("a box from a paint the glass never took is not 'already showing'");
  battpct::View v;
  v.hasRect = true;
  v.rectPlausible = true;
  v.canvasIsGlass = true;
  v.guardsClear = true;
  v.shownValue = 100;
  v.rectEpoch = 0;  // a held / declined paint: epoch 0, value says 100
  // The value matches, but the epoch proves the box is not on the glass -- so the
  // producer must WAIT (it costs nothing), not claim the number is already right
  // and not ask for a repaint.
  CHECK(battpct::need(v, 100) == battpct::Need::Pending);
  CHECK(battpct::need(v, 90) == battpct::Need::Pending);
  // The paint drives: the record is stamped and the value it drew is the current one.
  v.rectEpoch = 12;
  v.glassEpoch = 12;
  v.shownValue = 90;
  CHECK(battpct::need(v, 90) == battpct::Need::None);
}

void testWidthChangeAsksForRelayout() {
  section("a label whose width changes asks for a re-layout, not a patch");
  Model m = live(100);
  m.soc = 95;                // "100%" -> "95%": a digit fewer
  m.view.sameWidth = false;  // ...so the box on the glass is the wrong shape
  m.pass();
  CHECK(m.patches == 0);    // never drawn into a box that cannot hold it
  CHECK(m.repaints == 1);   // one re-layout instead
  // The re-layout recorded the new value at the new width; a same-width step after
  // it is back on the in-place path.
  m.view.sameWidth = true;
  m.soc = 94;
  m.pass();
  CHECK(m.patches == 1);
  CHECK(m.repaints == 1);
}

}  // namespace

int main() {
  testUnaddressableAsksOnce();
  testUnaddressableOnePerValue();
  testNoRequestWhileCleanArmed();
  testRateLimitUnderAlternation();
  testRateLimitIsGlobalAcrossSurfaces();
  testValueMemoryIsPerSurface();
  testSteadyValueIsFree();
  testOneChangeOnePatch();
  testBurstCollapses();
  testNoNumberOnScreenIsFree();
  testHeldPaintIsNotAGlassClaim();
  testWidthChangeAsksForRelayout();

  std::printf("\n%d/%d checks passed", checksRun - checksFailed, checksRun);
  if (checksFailed != 0) std::printf("  (%d FAILED)", checksFailed);
  std::printf("\n");
  return checksFailed == 0 ? 0 : 1;
}