#pragma once

#include <cstdint>

// Fixed-scalar active-reading summary. The owner reports turn outcomes before
// render and completed pages after render; no allocation or I/O occurs here.
//
// TWO READERS, ONE CLOCK (r163 3.3). The plugin event wants wall-clock start/end and a
// progress span, and only trusts epochs after a clock sync (isEmitWorthy). The
// reading-statistics page wants a time that exists on a card that never synced, so the
// same anchor pair also feeds a monotonic accumulator: activeMs advances by the dwell
// that preceded each successful forward turn, capped per dwell and ignoring sub-2 s
// skims; pages counts those turns. Both are conservative by construction: the last page
// of a session contributes no time, because no turn followed it.
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();

  bool isEmitWorthy() const { return startEpoch > 0 && duration >= 10 && endTime() > startEpoch; }
  int64_t startTime() const { return startEpoch; }
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + duration : 0; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

  // r163 3.3, statistics side: monotonic time and forward page turns since the last
  // reset. Neither needs a trusted epoch, so an unsynced card still records honest time.
  uint32_t activeMillis() const { return activeMs; }
  uint32_t pagesTurned() const { return pages; }
  bool hasActivity() const { return activeMs != 0 || pages != 0; }
  // Hands the counters to the caller and zeroes them, so a periodic checkpoint can persist
  // one window without disturbing the plugin-event summary (start/duration/progress).
  void takeStatistics(uint32_t& pagesOut, uint32_t& activeMsOut);

 private:
  static uint16_t clampProgress(int progressBp);

  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  // Monotonic side (no epoch needed). activeMs saturates at ~49 days, far past a session.
  uint32_t activeMs = 0;
  uint32_t pages = 0;
  bool hasAnchor = false;
  bool pendingForward = false;
};
