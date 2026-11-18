#include "util/BatteryStats.h"

#include <HalPowerManager.h>
#include <Logging.h>
#include <Preferences.h>
#include <ctime>

#include <cstdio>
#include <cstddef>
#include <cstring>

#include "display/EpdFlashGuard.h"

BatteryStats batteryStats;

namespace {
constexpr char NVS_NAMESPACE[] = "battstat";
constexpr char NVS_KEY[] = "rec";
constexpr uint32_t RECORD_MAGIC = 0x42535431u;  // 'BST1'
constexpr uint16_t RECORD_VERSION = 1;
// A wall clock earlier than this is "not usable" (no RTC and no NTP yet): the
// 2021 floor catches a chip that came up at epoch 0 or at an unset RTC date.
constexpr uint32_t PLAUSIBLE_EPOCH = 1600000000u;

uint32_t nowEpochSeconds() {
  const time_t t = time(nullptr);
  return static_cast<uint32_t>(t) >= PLAUSIBLE_EPOCH ? static_cast<uint32_t>(t) : 0u;
}

uint32_t fnv1a(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

// Saturating accumulators. A cycle's numbers run for as long as the pack lasts on
// battery, and a manual reset is the only thing that shortens the window -- but a
// stalled clock, a stuck charging flag or a very long charge must never turn a
// record into a wrapped one: uint32 milliseconds wrap after 49.7 days of
// accumulated state time and an int16 percentage sum wraps at 327 %. Both stop AT
// the ceiling instead.
constexpr uint32_t kMaxAccumMs = 3900000000u;  // ~45 days, inside uint32
constexpr int kMaxAccumPct = 30000;

uint32_t addMs(const uint32_t acc, const uint32_t add) {
  if (acc >= kMaxAccumMs) return kMaxAccumMs;
  return (add > kMaxAccumMs - acc) ? kMaxAccumMs : acc + add;
}

int16_t addPct(const int16_t acc, const int delta) {
  int v = static_cast<int>(acc) + delta;
  if (v > kMaxAccumPct) v = kMaxAccumPct;
  if (v < -kMaxAccumPct) v = -kMaxAccumPct;
  return static_cast<int16_t>(v);
}
}  // namespace

void BatteryStats::begin() {
  load();
  if (!loaded_) {
    // Fresh record: value-initialize everything except the header fields.
    rec_ = Record{};
    rec_.magic = RECORD_MAGIC;
    rec_.version = RECORD_VERSION;
    rec_.size = static_cast<uint16_t>(sizeof(Record));
    rec_.socAtLastChargeEnd = -1;
    loaded_ = true;
    dirty_ = true;
  }
  const uint32_t epoch = nowEpochSeconds();
  absorbGap(epoch);
  lastSampleMs_ = millis();
  lastSampleEpoch_ = epoch;
  haveSample_ = false;
  lastSoc_ = -1;
  // The estimate window is RAM-only, so a boot starts with no window at all: the
  // page says "cannot tell yet" until it has watched a discharge for ESTIMATE_MIN_SPAN_MS.
  winClear();
  chargingSinceMs_ = 0;
  edgePending_ = false;
  historyReset_ = false;
  postWakeWaitMs_ = 0;
  postWakeWaitGiveUp_ = false;
  publish();
  save(true);
}

void BatteryStats::load() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  Record stored{};
  const size_t got = prefs.getBytes(NVS_KEY, &stored, sizeof(stored));
  prefs.end();
  if (got != sizeof(stored)) return;
  // EVERY field of the guard has to agree. A record from another layout is
  // discarded rather than reinterpreted -- that is the whole point of storing
  // the size and the checksum next to the version.
  if (stored.magic != RECORD_MAGIC) return;
  if (stored.version != RECORD_VERSION) return;
  if (stored.size != sizeof(Record)) return;
  const uint32_t crc = fnv1a(reinterpret_cast<const uint8_t*>(&stored), offsetof(Record, crc));
  if (stored.crc != crc) return;
  rec_ = stored;
  loaded_ = true;
}

void BatteryStats::save(const bool force) {
  if (!loaded_) return;
  const uint32_t now = millis();
  if (!force && (now - lastPersistMs_) < PERSIST_MS) return;
  rec_.magic = RECORD_MAGIC;
  rec_.version = RECORD_VERSION;
  rec_.size = static_cast<uint16_t>(sizeof(Record));
  rec_.lastEpoch = lastSampleEpoch_ != 0 ? lastSampleEpoch_ : nowEpochSeconds();
  rec_.crc = fnv1a(reinterpret_cast<const uint8_t*>(&rec_), offsetof(Record, crc));
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    // The panel guard: an NVS commit is one erase + program and must not land
    // inside a waveform (EpdFlashGuard.h). A save is DELAYED, never dropped --
    // which is what makes the reset's write durable rather than best-effort.
    epdflash::NvsWriteScope guard("battstat");
    prefs.putBytes(NVS_KEY, &rec_, sizeof(rec_));
    prefs.end();
  }
  lastPersistMs_ = now;
  dirty_ = false;
}

// The gap between the previous session's last sample and this begin() is STANDBY
// time, not awake time. Below SLEEP_GAP_MIN_MS it is just a restart and is
// ignored; the first SoC delta after a real gap is then charged to SLEEP,
// because that is the state the battery actually spent the interval in.
void BatteryStats::absorbGap(const uint32_t nowEpoch) {
  if (rec_.lastEpoch == 0 || nowEpoch == 0 || nowEpoch <= rec_.lastEpoch) return;
  const uint32_t gapMs = (nowEpoch - rec_.lastEpoch) * 1000u;
  if (gapMs < SLEEP_GAP_MIN_MS) return;
  rec_.sleepMs = addMs(rec_.sleepMs, gapMs);
  rec_.pendingSleepDelta = 1;
  dirty_ = true;
  LOG_INF("BATT", "stats: wake gap %lus counted as sleep (awake=%lus sleep=%lus)", gapMs / 1000u,
          rec_.awakeMs / 1000u, rec_.sleepMs / 1000u);
}

void BatteryStats::update() {
  if (!loaded_) return;
  const uint32_t now = millis();
  if (haveSample_ && (now - lastSampleMs_) < SAMPLE_MS) return;
  // BATT6: DO NOT sample while the displayed percentage is still recovering from a
  // standby resume.
  //
  // After a wake the gauge machine holds the pre-sleep value for a 3 s settle and
  // then makes its ONE post-wake recalibration jump to the fresh target
  // (HalPowerManager::notifyStandbyWake -> gaugestate::SocMachine::wake, and
  // BatteryGaugeState.h's "samples during it are not evidence"). A sample taken
  // inside that window records the PRE-SLEEP value as the current one: the first
  // delta is then 0, the sleep's whole drop is consumed into nothing, and the drop
  // lands 30 s later as AWAKE discharge. That is the operator's "the statistics
  // record wrong samples" after a long standby, and it is an ordering fault, not a
  // gauge fault: the value exists, it just is not the number the machine is showing
  // yet.
  //
  // The wait is bounded twice over: the settle is 3 s, the poll that lifts it is
  // 1.5 s, and kPostWakeWaitMs stops waiting altogether if the gauge never answers
  // (a dead bus must not silently stop the awake-time accounting). Nothing is lost
  // by waiting: by the time this samples, lastSampleMs_ is still the pre-sleep one,
  // so the gap is still >= SLEEP_DETECT_MS and the interval is still booked to
  // SLEEP by sample()'s own gap rule.
  if (powerManager.socAwaitingPostWakeSample()) {
    if (postWakeWaitMs_ == 0) {
      postWakeWaitMs_ = now;
      LOG_INF("BATT", "stats: wake sample deferred until the post-wake SoC settles");
      return;
    }
    if ((now - postWakeWaitMs_) < kPostWakeWaitMs) return;  // still settling: no sample
    if (!postWakeWaitGiveUp_) {
      postWakeWaitGiveUp_ = true;
      LOG_INF("BATT",
              "stats: post-wake SoC still not settled after %lums -- sampling the held value (gauge not answering?)",
              static_cast<unsigned long>(kPostWakeWaitMs));
    }
  } else {
    postWakeWaitMs_ = 0;
    postWakeWaitGiveUp_ = false;
  }
  sample(now, nowEpochSeconds(), powerManager.getBatteryPercentage(), powerManager.chargingForStats());
}

void BatteryStats::sample(const uint32_t nowMs, const uint32_t nowEpoch, const uint16_t soc, const bool charging) {
  const uint32_t rawDt = haveSample_ ? (nowMs - lastSampleMs_) : 0;
  uint32_t clampedMs = rawDt;
  if (rawDt > SLEEP_DETECT_MS) {
    // The sampling gap is FAR longer than the period: the device was in standby.
    // An in-place standby wake (this board resumes with its framebuffer intact)
    // does not reboot, so millis() simply jumps forward by the sleep duration --
    // without this branch the whole interval would be booked as awake time and
    // "awake since last charge" would grow every night.
    rec_.sleepMs = addMs(rec_.sleepMs, rawDt - SAMPLE_MS);
    clampedMs = SAMPLE_MS;
    rec_.pendingSleepDelta = 1;
    dirty_ = true;
    LOG_INF("BATT", "stats: standby gap %lus counted as sleep", static_cast<unsigned long>(rawDt / 1000u));
  } else if (clampedMs > 10u * 60u * 1000u) {
    // A stall (a long blocking transfer, a debugger pause) is not a state the
    // battery sat in; cap what one tick may claim.
    clampedMs = 10u * 60u * 1000u;
  }

  const bool wasCharging = rec_.charging != 0;
  if (charging && chargingSinceMs_ == 0) chargingSinceMs_ = nowMs;
  if (charging && !wasCharging) {
    // A charge SESSION starts: the "during last charge" block describes one session,
    // so that block starts over. The awake/sleep/total record is NOT touched here --
    // it is cleared once, at the CONFIRMED unplug below, so the numbers keep counting
    // through the charge and the fresh cycle begins at the moment the cable came out.
    rec_.chargeMs = 0;
    rec_.chargeDelta = 0;
    edgePending_ = false;  // whatever edge was pending is void: we are charging again
    LOG_INF("BATT", "stats: charge start at %u%%", static_cast<unsigned>(soc));
  } else if (!charging && wasCharging) {
    // CANDIDATE charge -> discharge edge. This is the observable event the estimate
    // resets on: the charger IC's own status (HalPowerManager::chargingForStats ->
    // BQ27220/CX25601N charge flag) went non-zero -> zero, i.e. current stopped
    // flowing INTO the pack. Nothing about the record changes here.
    const uint32_t dwell = chargingSinceMs_ != 0 ? (nowMs - chargingSinceMs_) : 0;
    // Factual regardless of how long the charge lasted: this is when it stopped.
    rec_.socAtLastChargeEnd = static_cast<int16_t>(soc);
    if (dwell >= CHARGE_DWELL_MS) {
      // Armed, not taken: one glitched charger read must not throw the window away,
      // so the edge is only accepted when the NEXT sample still reports not-charging
      // (see the branch below). The edge's own SoC is kept so the fresh window starts
      // at the moment of unplug, not 30 s later.
      edgePending_ = true;
      edgeMs_ = nowMs;
      edgeSoc_ = soc;
      LOG_INF("BATT", "stats: charge end at %u%% after %lus (discharge window restarts on confirmation)",
              static_cast<unsigned>(soc), static_cast<unsigned long>(dwell / 1000u));
    } else {
      LOG_INF("BATT", "stats: charge end after %lus -- too short to be a charge cycle, window kept",
              static_cast<unsigned long>(dwell / 1000u));
    }
    chargingSinceMs_ = 0;
  } else if (!charging && edgePending_) {
    // CONFIRMED. Charging stopped and the next sample saw it stay stopped: this is a
    // real unplug, and it opens a new cycle. THREE THINGS RESTART HERE, from this one
    // edge, so the page describes the cycle that started at this unplug:
    //   1. the record (awake / sleeping / total, time and measured change),
    //   2. the charge session block,
    //   3. the rolling discharge window.
    // The operator's decision: an automatic reset beats remembering to press Reset,
    // so the page reads "since last charge" without intervention. Reset stays as a
    // manual override (see reset()).
    //
    // NOTHING WIPES HERE WITHOUT THE GATES ABOVE: a plug that never reached
    // CHARGE_DWELL_MS, or a charge flag that flickered for one sample, never gets
    // this far, so a cable blip cannot destroy the cycle's numbers.
    edgePending_ = false;
    historyReset_ = true;
    rec_.awakeMs = 0;
    rec_.sleepMs = 0;
    rec_.awakeDelta = 0;
    rec_.sleepDelta = 0;
    rec_.pendingSleepDelta = 0;  // a pending sleep delta belonged to the old cycle
    rec_.chargeMs = 0;
    rec_.chargeDelta = 0;
    winClear();
    winPush(edgeMs_, edgeSoc_);
    LOG_INF("BATT", "stats: discharge window restarted at %u%% (estimating until %lum)",
            static_cast<unsigned>(soc), static_cast<unsigned long>(ESTIMATE_MIN_SPAN_MS / 60000u));
    LOG_INF("BATT", "stats: cycle restarted at %u%% -- awake/sleep/total/charge cleared",
            static_cast<unsigned>(soc));
  }
  rec_.charging = charging ? 1 : 0;

  if (haveSample_ && lastSoc_ >= 0) {
    int delta = static_cast<int>(soc) - static_cast<int>(lastSoc_);
    if (delta > 100) delta = 100;
    if (delta < -100) delta = -100;
    if (charging) {
      rec_.chargeMs = addMs(rec_.chargeMs, clampedMs);
      rec_.chargeDelta = addPct(rec_.chargeDelta, delta);
    } else {
      rec_.awakeMs = addMs(rec_.awakeMs, clampedMs);
      if (rec_.pendingSleepDelta) {
        // The first delta after standby is what standby cost.
        rec_.sleepDelta = addPct(rec_.sleepDelta, delta);
        rec_.pendingSleepDelta = 0;
      } else {
        rec_.awakeDelta = addPct(rec_.awakeDelta, delta);
      }
    }
    if (delta != 0) dirty_ = true;
  } else if (haveSample_ && !charging) {
    rec_.awakeMs = addMs(rec_.awakeMs, clampedMs);
  } else if (haveSample_ && charging) {
    rec_.chargeMs = addMs(rec_.chargeMs, clampedMs);
  }

  // Feed the rate window. ON-BATTERY samples only (a charging sample describes the
  // wrong direction entirely), and none while an edge is still unconfirmed -- the
  // window is about to be re-seeded from that edge.
  if (!charging && !edgePending_) {
    winPush(nowMs, soc);
    winTrim(nowMs);
  }
  winCompute();

  lastSoc_ = static_cast<int16_t>(soc);
  lastSampleMs_ = nowMs;
  lastSampleEpoch_ = nowEpoch;
  haveSample_ = true;
  publish();
  // A cycle reset is forced, not throttled: the zeroed record has to be in NVS before
  // the next boot, or a power cut inside PERSIST_MS would resurrect the old cycle's
  // counters. Same one write, same EpdFlashGuard -- just not deferred.
  save(historyReset_);
  historyReset_ = false;
}

void BatteryStats::onEnteringSleep() {
  if (!loaded_) return;
  publish();
  // Commit the awake window as it stands, and let the next begin() find the gap.
  const uint32_t epoch = nowEpochSeconds();
  lastSampleEpoch_ = epoch;
  save(true);
  LOG_INF("BATT", "stats: entering sleep (awake=%lus sleep=%lus)", rec_.awakeMs / 1000u, rec_.sleepMs / 1000u);
}

void BatteryStats::reset() {
  const int16_t soc = static_cast<int16_t>(powerManager.getBatteryPercentage());
  rec_ = Record{};
  rec_.magic = RECORD_MAGIC;
  rec_.version = RECORD_VERSION;
  rec_.size = static_cast<uint16_t>(sizeof(Record));
  rec_.socAtLastChargeEnd = soc;
  loaded_ = true;
  dirty_ = true;
  haveSample_ = false;
  lastSoc_ = soc;
  lastSampleMs_ = millis();
  lastSampleEpoch_ = nowEpochSeconds();
  // The estimate is part of "everything": a cleared page must not keep printing a
  // drain rate measured before the clear. The window starts again from the next
  // sample, so the rate/time-left rows read "--" until it is long enough to trust.
  winClear();
  chargingSinceMs_ = 0;
  edgePending_ = false;
  historyReset_ = false;
  postWakeWaitMs_ = 0;
  postWakeWaitGiveUp_ = false;
  publish();
  save(true);
  LOG_INF("BATT", "stats: record reset at %d%% (history and estimate window cleared)", static_cast<int>(soc));
}

void BatteryStats::publish() {
  snap_.soc = lastSoc_ >= 0 ? static_cast<uint16_t>(lastSoc_) : powerManager.getBatteryPercentage();
  snap_.charging = rec_.charging != 0;
  snap_.awake.timeMs = rec_.awakeMs;
  snap_.awake.socDelta = rec_.awakeDelta;
  snap_.sleeping.timeMs = rec_.sleepMs;
  snap_.sleeping.socDelta = rec_.sleepDelta;
  snap_.charge.timeMs = rec_.chargeMs;
  snap_.charge.socDelta = rec_.chargeDelta;
  snap_.total.timeMs = rec_.awakeMs + rec_.sleepMs;
  snap_.total.socDelta = addPct(rec_.awakeDelta, rec_.sleepDelta);
  // The ESTIMATE. Gated three ways, so the page prints its existing "cannot tell
  // yet" rather than a figure that is not a measurement:
  //   * estReady_    -- the window spans >= ESTIMATE_MIN_SPAN_MS and has dropped
  //                     >= ESTIMATE_MIN_DROP since the last charge->discharge edge;
  //   * !charging    -- a stale discharge window must not be shown as "time left"
  //                     while the pack is filling;
  //   * timeMs == 0  -- the signal haveRate() reads, so the activity needs no new
  //                     branch: an unready window is an empty Section.
  Section win{};
  if (estReady_ && !snap_.charging) {
    win.timeMs = estSpanMs_;
    win.socDelta = estDelta_;
  }
  snap_.discharge = win;
}

// ---------------------------------------------------------------- rate window --
// The estimate, not the record. See the constants in the header for why 2 h / 45 min
// / 2 %.

void BatteryStats::winClear() {
  winHead_ = 0;
  winCount_ = 0;
  estSpanMs_ = 0;
  estDelta_ = 0;
  estReady_ = false;
}

void BatteryStats::winPush(const uint32_t tMs, const uint16_t soc) {
  win_[winHead_].tMs = tMs;
  win_[winHead_].soc = soc;
  winHead_ = (winHead_ + 1) % WINDOW_CAP;
  if (winCount_ < WINDOW_CAP) ++winCount_;
}

// Drop samples that have fallen out of the window. Indices are DERIVED from
// (head, count), so dropping the oldest is just a decrement -- nothing moves.
void BatteryStats::winTrim(const uint32_t nowMs) {
  while (winCount_ > 0) {
    const int oldest = (winHead_ - winCount_ + WINDOW_CAP) % WINDOW_CAP;
    if ((nowMs - win_[oldest].tMs) <= WINDOW_MS) break;
    --winCount_;
  }
}

void BatteryStats::winCompute() {
  estSpanMs_ = 0;
  estDelta_ = 0;
  estReady_ = false;
  if (winCount_ < 2) return;
  const int oldest = (winHead_ - winCount_ + WINDOW_CAP) % WINDOW_CAP;
  const int newest = (winHead_ - 1 + WINDOW_CAP) % WINDOW_CAP;
  const uint32_t span = win_[newest].tMs - win_[oldest].tMs;
  const int delta = static_cast<int>(win_[newest].soc) - static_cast<int>(win_[oldest].soc);
  estSpanMs_ = span;
  estDelta_ = static_cast<int16_t>(delta < -32767 ? -32767 : (delta > 32767 ? 32767 : delta));
  // Three conditions, and all three are about the measurement being real rather than
  // about the battery being empty: the window must span enough time for a 1 % step
  // not to dominate, it must have moved at least two steps, and the movement must be
  // DOWNWARD -- a rising window while nominally on battery is gauge relaxation or a
  // warm pack, not a drain, and must never print a "time left".
  estReady_ = span >= ESTIMATE_MIN_SPAN_MS && delta <= -static_cast<int>(ESTIMATE_MIN_DROP);
}

bool BatteryStats::haveRate(const Section& section) {
  return section.timeMs >= MIN_RATE_WINDOW_MS && section.socDelta != 0;
}

float BatteryStats::perHour(const Section& section) {
  if (!haveRate(section)) return 0.0f;
  const float hours = static_cast<float>(section.timeMs) / 3600000.0f;
  return static_cast<float>(section.socDelta) / hours;
}

const char* BatteryStats::formatDuration(char* buf, const size_t size, const uint32_t ms) {
  if (buf == nullptr || size == 0) return buf;
  const uint32_t totalMinutes = ms / 60000u;
  const uint32_t hours = totalMinutes / 60u;
  const uint32_t minutes = totalMinutes % 60u;
  if (hours > 0) {
    snprintf(buf, size, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, size, "%lum", static_cast<unsigned long>(minutes));
  }
  return buf;
}
