#pragma once

// r97 BATT2 -- battery statistics accounting.
//
// KOReader-style bookkeeping, but measured rather than extrapolated: the state
// layer integrates real elapsed time per power state (awake / sleeping /
// charging) and the REAL gauge SoC delta observed over that same window, so
// "change per hour" is (delta / elapsed), never a guess from capacity.
//
// NOTHING here is derived from mAh: designCapacityMah is 0 on this board and
// loadDesignCapacity() is never called, so every number on the page is either a
// percentage the gauge reported or a time we measured. That is deliberate -- a
// fabricated capacity would turn a measurement into a fiction.
//
// Persistence: ONE versioned NVS record (namespace "battstat", key "rec"). The
// record carries magic + version + its own written size + a checksum, and a
// record that fails any of those is DISCARDED rather than reinterpreted, so a
// future layout change cannot resurrect old bytes as new fields.
//
// HISTORY vs ESTIMATE (r101). The record above is HISTORY: awake/sleeping time and
// the SoC change across them, for the cycle that started at the last confirmed
// unplug. The rate and time-left the page prints are an ESTIMATE, computed from a
// rolling window of the last WINDOW_MS of on-battery samples.
//
// ONE EDGE, THREE RESTARTS. Both the estimate window and the record restart on the
// same gated event -- the charger IC's status going non-zero -> zero, held for
// >= CHARGE_DWELL_MS and confirmed by the next sample. The operator's call: an
// automatic reset beats remembering to press Reset, so the page reads "since last
// charge" with no intervention, and every block describes the cycle that began at
// that unplug. reset() remains as a manual override.

#include <cstdint>

class BatteryStats {
 public:
  // One block of the page: a state, the measured time in it, and the gauge
  // delta observed across exactly that time.
  struct Section {
    uint32_t timeMs = 0;
    int16_t socDelta = 0;  // net percentage change (negative = discharge)
  };

  struct Snapshot {
    uint16_t soc = 0;
    bool charging = false;
    Section awake;    // awake, since the last cycle reset (unplug) or manual RESET
    Section sleeping; // standby, since the last cycle reset (unplug) or manual RESET
    Section charge;   // the whole of the last charge session
    Section total;    // awake + sleeping, since the last cycle reset or manual RESET
    // THE ESTIMATE, and the only number on the page that is not a record: the drain
    // measured over the last WINDOW_MS of ON-BATTERY samples, re-seeded on every
    // confirmed charge -> discharge edge. Emptied (timeMs = socDelta = 0) until the
    // window can support a rate, so haveRate()/perHour() answer "cannot tell yet"
    // for it exactly as they do for a section that has barely started.
    Section discharge;
  };

  static constexpr uint32_t SAMPLE_MS = 30000;      // gauge/state sampling period
  static constexpr uint32_t PERSIST_MS = 300000;    // NVS write period while running
  static constexpr uint32_t MIN_RATE_WINDOW_MS = 600000;  // "too short to rate" below 10 min
  static constexpr uint32_t SLEEP_GAP_MIN_MS = 120000;    // a reboot gap shorter than this is not sleep
  static constexpr uint32_t SLEEP_DETECT_MS = 90000;      // a sampling gap this long IS standby, not awake

  // --- the rolling discharge window (the estimate, never the record) ----------
  //
  // 2 h. The gauge reports whole percent, so a rate is only as good as the SoC step
  // it is measured across: a 10-minute window showing one 1 % step would print
  // -6 %/h, which is the "estimates drift" report. Two hours is long enough that
  // the quantisation is a fraction of the figure at a real 1-2 %/h drain, and short
  // enough that the number tracks THIS session instead of averaging everything
  // since the last charge (the old lifetime denominator -- that was the bug).
  static constexpr uint32_t WINDOW_MS = 7200000;
  // A window shorter than 45 min cannot carry a figure: 45 min is the shortest span
  // in which a single 1 % gauge step is under 1.4 %/h, and a 1 % step must never be
  // the whole measurement.
  static constexpr uint32_t ESTIMATE_MIN_SPAN_MS = 2700000;
  // ...and a window must have moved at least 2 %, i.e. hold at least two steps, so
  // one dithering 1 % read cannot manufacture a rate. At a 1-2 %/h drain the
  // estimate therefore first appears after ~1-2 h of discharge, and says "--"
  // (the page's existing "cannot tell yet") until then.
  static constexpr int16_t ESTIMATE_MIN_DROP = 2;
  // Charging for less than this is a cable blip, not a charge cycle; its falling
  // edge must not throw the discharge window away.
  static constexpr uint32_t CHARGE_DWELL_MS = 300000;
  // BATT6: how long a post-wake sample may be deferred waiting for the gauge's
  // one-shot recalibration. The settle is 3 s and the poll is 1.5 s, so a healthy
  // gauge lifts this within ~4.5 s; the ceiling only exists so a dead gauge cannot
  // stop the awake-time accounting forever.
  static constexpr uint32_t kPostWakeWaitMs = 15000;

  void begin();
  // Sample + integrate. Call from the main loop; internally throttled to
  // SAMPLE_MS. `soc` and `charging` are read fresh on the tick.
  void update();
  // Called on the way into standby: commits the record and closes the awake
  // window, so the time until the next begin() is counted as SLEEP.
  void onEnteringSleep();
  // Wipe the record AND the estimate window and start again. This is the MANUAL
  // OVERRIDE, not the primary mechanism: the record already restarts on its own at
  // every confirmed charge -> discharge edge (see sample()). The button stays
  // because it is the only way to clear the numbers mid-cycle, e.g. after handing
  // the device to someone else or to start measuring from a known state. Every
  // number the page shows returns to a just-booted state, and the empty record is
  // written to NVS so a reboot cannot resurrect the old one.
  void reset();

  const Snapshot& snapshot() const { return snap_; }
  bool valid() const { return loaded_; }

  static const char* formatDuration(char* buf, size_t size, uint32_t ms);
  // Percent-per-hour for a section, or NAN-free "cannot tell yet" via haveRate.
  static bool haveRate(const Section& section);
  static float perHour(const Section& section);

 private:
  struct Record {
    uint32_t magic;
    uint16_t version;
    uint16_t size;  // sizeof(Record) as written
    uint32_t awakeMs;
    uint32_t sleepMs;
    int16_t awakeDelta;
    int16_t sleepDelta;
    uint32_t chargeMs;
    int16_t chargeDelta;
    int16_t socAtLastChargeEnd;
    uint32_t lastEpoch;  // wall clock at the last sample; 0 = clock was not usable
    uint8_t charging;    // the state believed at the last sample
    uint8_t pendingSleepDelta;  // the next delta belongs to SLEEP (we just woke)
    uint32_t crc;
  };

  void load();
  void save(bool force);
  void absorbGap(uint32_t nowEpoch);
  void sample(uint32_t nowMs, uint32_t nowEpoch, uint16_t soc, bool charging);
  void publish();

  // --- rolling discharge window ----------------------------------------------
  // A FIFO ring of (millis, SoC) pairs, appended once per SAMPLE_MS while running
  // on battery and trimmed to WINDOW_MS from the newest sample. Fixed size, so the
  // estimator's memory is the same after a month as after a minute (see the
  // retention note in the header comment). RAM-only on purpose: it describes the
  // last two hours, and a reboot legitimately has no such history to show.
  struct WinSample {
    uint32_t tMs = 0;
    uint16_t soc = 0;
  };
  // One slot per SAMPLE_MS of window, plus a little slack for a stall-truncated
  // tick; the ring is a FIFO, so an overflow only drops the oldest sample.
  static constexpr int WINDOW_CAP = static_cast<int>(WINDOW_MS / SAMPLE_MS) + 4;

  void winClear();
  void winPush(uint32_t tMs, uint16_t soc);
  void winTrim(uint32_t nowMs);
  void winCompute();

  Record rec_{};
  Snapshot snap_{};
  bool loaded_ = false;
  bool dirty_ = false;
  bool haveSample_ = false;
  int16_t lastSoc_ = -1;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastSampleEpoch_ = 0;
  uint32_t lastPersistMs_ = 0;

  WinSample win_[WINDOW_CAP];
  int winHead_ = 0;   // next write slot
  int winCount_ = 0;
  uint32_t estSpanMs_ = 0;   // span the window covers, whether or not it may rate
  int16_t estDelta_ = 0;
  bool estReady_ = false;

  // Charge -> discharge edge bookkeeping. A falling charge flag is only accepted as
  // a real unplug once (a) the charge had lasted CHARGE_DWELL_MS and (b) the very
  // next sample still reports not-charging, so one glitched charger read cannot
  // discard the cycle.
  uint32_t chargingSinceMs_ = 0;  // 0 = not charging
  bool edgePending_ = false;
  uint32_t edgeMs_ = 0;
  uint16_t edgeSoc_ = 0;
  // BATT6: the post-wake sample deferral (see update()). 0 = not waiting.
  uint32_t postWakeWaitMs_ = 0;
  bool postWakeWaitGiveUp_ = false;
  // Set by the confirmed edge, consumed by the end of that same sample(): the record
  // it just zeroed is written with force, so a reboot one second later cannot read
  // the previous cycle back out of NVS.
  bool historyReset_ = false;
};

extern BatteryStats batteryStats;
