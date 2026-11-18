#include "ReaderSession.h"

#include <algorithm>

#include "util/ReadingStats.h"

namespace {
// One source of truth for both accumulators: the statistics module's dwell window.
constexpr uint32_t MIN_DWELL_MS = readingstats::kMinDwellMs;
constexpr uint32_t MAX_DWELL_MS = readingstats::kMaxDwellMs;
}  // namespace

uint16_t ReaderSession::clampProgress(const int progressBp) {
  return static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
}

void ReaderSession::noteTurn(const bool forward, const bool succeeded) { pendingForward = forward && succeeded; }

void ReaderSession::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch, const int progressBp) {
  const uint16_t progress = clampProgress(progressBp);
  const bool turn = pendingForward;
  const uint32_t dwellMs = hasAnchor ? monotonicMs - anchorMs : 0;

  // Statistics side: a successful forward turn is one page, and the dwell that preceded
  // it is reading time -- provided it was a read and not a skim. Runs independently of
  // the epoch, so an unsynced card still records honest time.
  if (turn) {
    if (pages < 1'000'000u) ++pages;
    if (hasAnchor && dwellMs >= MIN_DWELL_MS) {
      const uint32_t add = std::min(dwellMs, MAX_DWELL_MS);
      // Saturate rather than wrap: a session open for weeks must read as "very long",
      // never as a small number.
      activeMs = activeMs > UINT32_MAX - add ? UINT32_MAX : activeMs + add;
    }
  }

  // Plugin-event side: wall-clock span, only from a trusted epoch.
  if (turn && hasAnchor && anchorEpoch > 0) {
    const uint32_t elapsedSeconds = dwellMs / 1000;
    if (elapsedSeconds >= MIN_DWELL_MS / 1000) {
      const uint32_t contribution = std::min(elapsedSeconds, MAX_DWELL_MS / 1000);
      if (duration == 0) {
        startEpoch = anchorEpoch;
        startProgress = anchorProgress;
        endProgress = anchorProgress;
      }
      duration += contribution;
      endProgress = std::max(endProgress, progress);
    }
  }

  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  anchorProgress = progress;
  hasAnchor = true;
  pendingForward = false;
}

void ReaderSession::takeStatistics(uint32_t& pagesOut, uint32_t& activeMsOut) {
  pagesOut = pages;
  activeMsOut = activeMs;
  pages = 0;
  activeMs = 0;
}

void ReaderSession::reset() {
  anchorMs = 0;
  anchorEpoch = 0;
  startEpoch = 0;
  duration = 0;
  anchorProgress = 0;
  startProgress = 0;
  endProgress = 0;
  activeMs = 0;
  pages = 0;
  hasAnchor = false;
  pendingForward = false;
}
