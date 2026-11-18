// AUDIO1 — short UI feedback sounds. See UiSounds.h for the contract and
// UiSoundPolicy.h for the decisions (which live outside this file so the host
// test compiles the real thing).
//
// THREADING
//   UI task  : onEvent() only. spinlock + counter + one semaphore give.
//   worker   : claim -> btaudio::playPcm -> completed. Owns every blocking call.
//   The two never share a blocking primitive, in either direction.

#include "audio/UiSounds.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "Logging.h"
#include <PersistableStore.h>
#include "audio/BtAudioOutput.h"
#include "platform/StandbyTrims.h"

namespace uisounds {
namespace {

// All waveforms are 16 kHz mono (the btaudio path's rate) and are built once.
constexpr uint16_t kRate = 16000;
constexpr int kMaxSamples = 3200;  // 200 ms TOTAL for all five waves (see the static_assert)
int16_t g_pcm[kMaxSamples];

struct Wave {
  uint16_t off = 0;
  uint16_t len = 0;
};
Wave g_wave[static_cast<int>(Sound::Count)];

// Featured gently, never startling: peaks sit around 15-22 % of full scale.
constexpr double kTwoPi = 6.283185307179586;

// Cosine fade over the first/last `edge` samples: a hard start on a DAC is an
// audible thump, not a click.
double fade(const uint32_t i, const uint32_t n, const uint32_t edge) {
  if (edge == 0) return 1.0;
  if (i < edge) return 0.5 - 0.5 * cos(double(i) / double(edge) * kTwoPi / 2.0);
  if (i + edge >= n) return 0.5 - 0.5 * cos(double(n - 1 - i) / double(edge) * kTwoPi / 2.0);
  return 1.0;
}

struct Spec {
  uint16_t ms;
  double f0, f1;   // linear sweep f0 -> f1 (equal for a plain tone)
  double decayMs;  // exponential decay constant; 0 = flat envelope
  double gain;     // fraction of full scale
  bool doubleHit;  // restrike in the second half (the error tone)
};

// One entry per Sound, in enum order. Lengths come from UiSoundPolicy.h's
// kSoundMs (the host test models the busy window against the same table, and the
// static_assert below pins them together); pitches are chosen so the five sounds
// are instantly distinguishable from each other and all land well under 100 ms:
// feedback, not a soundtrack.
constexpr Spec kSpecs[static_cast<int>(Sound::Count)] = {
    /* Click  page turn   */ {kSoundMs[0], 1100, 1100, 7.0, 0.20, false},
    /* Move   menu move   */ {kSoundMs[1], 1900, 1900, 3.0, 0.15, false},
    /* Select selection   */ {kSoundMs[2], 950, 1450, 26.0, 0.20, false},
    /* Back   one level   */ {kSoundMs[3], 1250, 800, 22.0, 0.18, false},
    /* Error  disallowed  */ {kSoundMs[4], 300, 300, 0.0, 0.16, true},
};

// The table is shared, so a spec edit that overflows it would silently truncate
// the LAST sounds in buildWaves(). This makes that a compile error instead.
constexpr uint32_t totalWaveFrames() {
  uint32_t n = 0;
  for (int s = 0; s < static_cast<int>(Sound::Count); s++) n += (uint32_t(kRate) * kSpecs[s].ms) / 1000;
  return n;
}
static_assert(totalWaveFrames() <= uint32_t(kMaxSamples), "UI waveform table overflows g_pcm");

// AUDIO3: the busy window a press can cost is its own wave length, so the
// longest wave is the worst-case policy hold. 100 ms is the contract (feedback,
// not a soundtrack) and the host test's "10 presses 20 ms apart" case reads it.
constexpr uint16_t longestWaveMs() {
  uint16_t n = 0;
  for (int s = 0; s < static_cast<int>(Sound::Count); s++) n = kSoundMs[s] > n ? kSoundMs[s] : n;
  return n;
}
static_assert(longestWaveMs() <= 100, "a UI sound must stay under 100 ms");


void buildWaves() {
  uint16_t cursor = 0;
  for (int s = 0; s < static_cast<int>(Sound::Count); s++) {
    const Spec& sp = kSpecs[s];
    uint32_t n = (uint32_t(kRate) * sp.ms) / 1000;
    if (n > uint32_t(kMaxSamples - cursor)) n = uint32_t(kMaxSamples - cursor);
    g_wave[s].off = cursor;
    g_wave[s].len = static_cast<uint16_t>(n);
    const uint32_t edge = uint32_t(kRate) * 2 / 1000;  // 2 ms
    double phase = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const double t = double(i) / double(kRate);
      const double f = sp.f0 + (sp.f1 - sp.f0) * (double(i) / double(n));
      phase += f / double(kRate);
      double env = fade(i, n, edge);
      if (sp.decayMs > 0.0) env *= exp(-t * 1000.0 / sp.decayMs);
      if (sp.doubleHit) {
        // 0-30 ms strike, 30-45 ms gap, 45-75 ms strike.
        const double ms = t * 1000.0;
        const double hit = (ms < 30.0) ? 1.0 : (ms < 45.0 ? 0.0 : 1.0);
        env *= hit;
      }
      const double v = sp.gain * env * sin(phase * kTwoPi);
      g_pcm[cursor + i] = static_cast<int16_t>(v * 32767.0);
    }
    cursor = static_cast<uint16_t>(cursor + n);
  }
}

SemaphoreHandle_t g_sem = nullptr;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
Policy g_policy;

// AUDIO3 — the instruments. All three live under g_mux (the same lock that
// guards the policy) so a dump can never read a half-updated row, and none of
// them can influence a decision: `g_ledger` is written after `submit()` has
// already returned its answer.
Ledger g_ledger{};

constexpr int kTraceDepth = 32;
TraceEntry g_trace[kTraceDepth];
int g_traceHead = 0;   // next slot to write
int g_traceCount = 0;  // valid entries, saturating at kTraceDepth
uint32_t g_traceMs[kTraceDepth];  // millis() of each entry's write

std::atomic<bool> g_traceOn{false};
std::atomic<uint32_t> g_suppressed{0};

// One traced line per event, but never more than one per kTraceMinGapMs. A
// burst is therefore a COUNT in the next line, not a flood: a held key at 13 ms
// per repeat would otherwise put ~75 lines/s on the CDC and stall the very UI
// task the sound is supposed to reassure.
constexpr uint32_t kTraceMinGapMs = 200;
std::atomic<uint32_t> g_lastTraceLogMs{0};  // written by the UI task, read by setTrace()
std::atomic<bool> g_traceLoggedOnce{false};

std::atomic<bool> g_armed{false};  // init() completed
std::atomic<bool> g_ui{false};     // SETTINGS.uiSoundsEnabled
std::atomic<bool> g_pathOk{true};  // worker has not quarantined the path
std::atomic<uint8_t> g_workerState{0};
std::atomic<uint32_t> g_failed{0};
std::atomic<uint32_t> g_lastCostUs{0};
std::atomic<uint16_t> g_lastFrames{0};
char g_reason[48] = "not initialised";

void setReason(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void setReason(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_reason, sizeof(g_reason), fmt, ap);
  va_end(ap);
}

// Worker task only. Never called from onEvent().
bool playWave(Sound s) {
  const int idx = static_cast<int>(s);
  if (idx < 0 || idx >= static_cast<int>(Sound::Count)) return false;
  const Wave& w = g_wave[idx];
  if (w.len == 0) return false;
  g_workerState.store(2);
  g_lastFrames.store(w.len);
  const bool ok = btaudio::playPcm(g_pcm + w.off, w.len);
  g_workerState.store(1);
  if (!ok) {
    g_failed.fetch_add(1);
    // Quarantine ONLY when the path itself is down. `playPcm` also returns false
    // when another tone holds the audio lock; that is transient and must not
    // silence the UI for the rest of the session.
    const auto st = btaudio::status();
    if (st.state == btaudio::State::Faulted || st.state == btaudio::State::Disabled) {
      g_pathOk.store(false);
      setReason("path %s -- clicks off until re-enabled", btaudio::stateName(st.state));
    }
  }
  return ok;
}

// AUDIO3 — the heartbeat. The worker is the only task that may log about the
// counters without touching the input path, and its semaphore wait is the one
// place that is idle anyway. Every kHeartbeatMs (when it is NOT draining a
// sound) it re-reads the counters and, if any of them moved, prints ONE line
// with the deltas. Silent when nothing happened, so an idle reader sees no
// traffic at all.
constexpr uint32_t kHeartbeatMs = 2000;
uint32_t g_hbStarted = 0, g_hbQueued = 0, g_hbDropped = 0, g_hbSkipped = 0;

void workerTask(void*) {
  g_workerState.store(1);
  for (;;) {
    if (xSemaphoreTake(g_sem, pdMS_TO_TICKS(kHeartbeatMs)) != pdTRUE) {
      uint32_t s, q, d, k;
      portENTER_CRITICAL(&g_mux);
      s = g_policy.started;
      q = g_policy.queued;
      d = g_policy.dropped;
      k = g_policy.skipped;
      portEXIT_CRITICAL(&g_mux);
      if (s != g_hbStarted || q != g_hbQueued || d != g_hbDropped || k != g_hbSkipped) {
        LOG_INF("AUDIO", "uisnd counters started=%u (+%u) queued=%u (+%u) dropped=%u (+%u) skipped=%u (+%u)",
                unsigned(s), unsigned(s - g_hbStarted), unsigned(q), unsigned(q - g_hbQueued), unsigned(d),
                unsigned(d - g_hbDropped), unsigned(k), unsigned(k - g_hbSkipped));
        g_hbStarted = s;
        g_hbQueued = q;
        g_hbDropped = d;
        g_hbSkipped = k;
      }
      continue;
    }
    // Drain: claim -> play -> completed. `completed()` promotes the one-deep
    // queue slot, so the next claim() picks it up without waiting again.
    for (;;) {
      int ev;
      portENTER_CRITICAL(&g_mux);
      ev = g_policy.claim();
      portEXIT_CRITICAL(&g_mux);
      if (ev < 0) break;
      // The standby park owns P1.4: a write now would fight it and fault the
      // session. Drop the sound instead (the event already counted as started).
      if (!standbytrim::parked()) playWave(static_cast<Sound>(ev));
      portENTER_CRITICAL(&g_mux);
      g_policy.completed();
      portEXIT_CRITICAL(&g_mux);
    }
    g_workerState.store(1);
  }
}

}  // namespace

void init() {
  if (g_armed.load()) return;
  buildWaves();
  g_ui.store(SETTINGS.uiSoundsEnabled != 0);
  // TTS1's init() is documented as boot-path safe: it creates the audio mutex
  // and reads the persisted gate. No rail, no UART, no I2S. Calling it here
  // means btaudio::gate() is already accurate when the first click arrives.
  btaudio::init();
  g_sem = xSemaphoreCreateBinary();
  if (g_sem != nullptr) {
    // Priority floor: a click must never preempt the UI or the render task.
    if (xTaskCreate(workerTask, "uisnd", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr) == pdPASS) {
      g_armed.store(true);
      setReason("armed; ui=%d", int(g_ui.load()));
    } else {
      vSemaphoreDelete(g_sem);
      g_sem = nullptr;
      setReason("worker task creation failed -- no clicks");
    }
  } else {
    setReason("worker semaphore allocation failed");
  }
  LOG_INF("AUDIO", "uisounds init: ui=%d master=%d waves=%u bytes", int(g_ui.load()), int(btaudio::gate()),
          unsigned(sizeof(g_pcm)));
}

void onEvent(Event e, Tag tag) {
  if (!g_armed.load(std::memory_order_relaxed)) return;
  const int64_t t0 = esp_timer_get_time();

  // Two atomic reads and one plain bool: the master gate is TTS1's own atomic,
  // the UI switch is ours, `parked()` is the standby park's flag (true from the
  // moment main.cpp starts parking until resume). No lock taken from any other
  // module, nothing that can wait.
  const bool master = btaudio::gate();
  const bool ui = g_ui.load(std::memory_order_relaxed);
  const bool parked = standbytrim::parked();  // never during the sleep transition

  // Read the clock OUTSIDE the critical section: the trace entry needs a stamp,
  // and a spinlock should hold integer assignments and nothing else.
  const uint32_t nowMs = static_cast<uint32_t>(millis());

  Policy::Outcome outcome;
  portENTER_CRITICAL(&g_mux);
  g_policy.master = master;
  g_policy.ui = ui;
  g_policy.pathReady = !parked && g_pathOk.load(std::memory_order_relaxed);
  outcome = g_policy.submit(e);
  // AUDIO3: attribute the outcome to the surface that asked for it, and keep the
  // last kTraceDepth calls verbatim. Recorded AFTER submit() decided, under the
  // policy's own lock, so the ledger cannot race the counters it explains.
  g_ledger.record(tag, e, outcome);
  {
    TraceEntry& t = g_trace[g_traceHead];
    t.tag = static_cast<uint8_t>(tag);
    t.event = static_cast<uint8_t>(e);
    t.outcome = static_cast<uint8_t>(Ledger::indexOf(outcome));
    t.ageMs = 0;  // filled in at dump time from g_traceMs (a 16-bit age would wrap otherwise)
    g_traceMs[g_traceHead] = nowMs;
    g_traceHead = (g_traceHead + 1) % kTraceDepth;
    if (g_traceCount < kTraceDepth) g_traceCount++;
  }
  portEXIT_CRITICAL(&g_mux);

  if (outcome == Policy::Outcome::Started) {
    if (g_sem != nullptr) xSemaphoreGive(g_sem);
  }

  // Trace line: opt-in, and rate-limited to one per kTraceMinGapMs. The
  // suppressed count rides on the next line so a burst is still accounted for,
  // and the whole block is skipped when the trace is off -- which is the default
  // and the state the event path was cost-measured in.
  if (g_traceOn.load(std::memory_order_relaxed)) {
    const uint32_t last = g_lastTraceLogMs.load(std::memory_order_relaxed);
    const bool emit = !g_traceLoggedOnce.load(std::memory_order_relaxed) || (nowMs - last) >= kTraceMinGapMs;
    if (emit) {
      const uint32_t suppressed = g_suppressed.exchange(0);
      g_lastTraceLogMs.store(nowMs, std::memory_order_relaxed);
      g_traceLoggedOnce = true;
      LOG_INF("AUDIO", "uisnd %s/%s -> %s (suppressed %u)", tagName(tag), eventName(e),
              Ledger::outcomeName(Ledger::indexOf(outcome)), unsigned(suppressed));
    } else {
      g_suppressed.fetch_add(1);
    }
  }

  g_lastCostUs.store(uint32_t(esp_timer_get_time() - int64_t(t0)));
}

// AUDIO3: the cached switch moves, the SETTINGS byte moves, and that is ALL.
// Persisting belongs to whoever owns the setting: SettingsActivity already saves
// once per change (applyUiSettingChange's callers), so a save here made one tap
// write the settings file twice. The shell path saves explicitly instead.
bool setEnabled(bool on) {
  if (!g_armed.load()) init();
  SETTINGS.uiSoundsEnabled = on ? 1 : 0;
  g_ui.store(on);
  g_pathOk.store(true);  // re-arm the worker's verdict
  setReason(on ? "ui sounds on" : "ui sounds off");
  LOG_INF("AUDIO", "uisounds enabled=%d", int(on));
  return true;
}

bool enabled() { return g_ui.load(); }

bool playNow(Sound s) {
  if (!g_armed.load()) init();
  if (!btaudio::gate()) {
    setReason("playNow refused: master gate off");
    return false;
  }
  if (!g_ui.load()) {
    setReason("playNow refused: ui sounds off");
    return false;
  }
  if (standbytrim::parked()) {
    setReason("playNow refused: device is parked");
    return false;
  }
  return playWave(s);
}

Stats stats() {
  Stats st = {};
  st.master = btaudio::gate();
  st.ui = g_ui.load();
  st.armed = g_armed.load();
  st.pathOk = g_pathOk.load();
  st.workerState = g_workerState.load();
  portENTER_CRITICAL(&g_mux);
  st.started = g_policy.started;
  st.queued = g_policy.queued;
  st.dropped = g_policy.dropped;
  st.skipped = g_policy.skipped;
  portEXIT_CRITICAL(&g_mux);
  st.failed = g_failed.load();
  st.lastCostUs = g_lastCostUs.load();
  st.lastFrames = g_lastFrames.load();
  st.traceOn = g_traceOn.load() ? 1u : 0u;
  st.saves = persiststore::saveCount();
  snprintf(st.reason, sizeof(st.reason), "%s", g_reason);
  return st;
}

const Ledger& ledger() { return g_ledger; }

int trace(TraceEntry* out, int maxOut) {
  if (out == nullptr || maxOut <= 0) return 0;
  int n = 0;
  const uint32_t now = static_cast<uint32_t>(millis());
  portENTER_CRITICAL(&g_mux);
  const int total = g_traceCount;
  for (int i = 0; i < total && n < maxOut; i++) {
    // Oldest first: the ring's head is the next slot to write.
    const int idx = (g_traceHead - total + i + kTraceDepth * 2) % kTraceDepth;
    TraceEntry e = g_trace[idx];
    const uint32_t age = now - g_traceMs[idx];
    e.ageMs = static_cast<uint16_t>(age > 60000u ? 60000u : age);
    out[n++] = e;
  }
  portEXIT_CRITICAL(&g_mux);
  return n;
}

void setTrace(bool on) {
  g_traceOn.store(on);
  if (on) {
    g_traceLoggedOnce = false;  // the first line after enabling is always written
    g_suppressed.store(0);
  }
  LOG_INF("AUDIO", "uisnd trace %s", on ? "ON (one line per %ums max)" : "OFF");
}

bool traceOn() { return g_traceOn.load(); }

}  // namespace uisounds
