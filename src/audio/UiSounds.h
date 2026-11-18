#pragma once

// ---------------------------------------------------------------------------
// AUDIO1 — short UI feedback sounds on the TTS1 output path.
//
// THE ONE RULE: a click that delays a paint is worse than silence.
//
//   onEvent() is the ONLY thing the UI task ever calls, and it is a bounded
//   handful of instructions: two atomic loads, a spinlock around a few integer
//   assignments, and at most one xSemaphoreGive. It never touches I2S, UART,
//   the TCA, the rail, the heap, or a blocking mutex. The audio work happens on
//   a dedicated worker task, so a slow or absent DAC costs the reader nothing.
//
//   The waveforms are generated ONCE at init() (a few hundred samples each) and
//   played from static memory. Nothing is synthesised on the event path and
//   nothing is allocated after init.
//
// GATING (both must be on, both default OFF)
//   SETTINGS.audioOutEnabled  — TTS1's existing master gate for the whole path.
//   SETTINGS.uiSoundsEnabled  — this feature's own switch, so enabling audio for
//                               a future feature (TTS, media) does not make the
//                               UI noisy.
//   With either one off, or while the path is faulted / the device is parked /
//   the module has not been armed, onEvent() returns after a counter bump. It
//   does not retry, does not log per event, and does not touch hardware.
//
// REUSE, NOT REIMPLEMENTATION
//   Playback goes through btaudio::playPcm(), which shares TTS1's lock, power-on
//   handshake, DMA chunking and fault policy. This module never drives P1.4,
//   P0.4 or I2S itself and has no idea how the chip is brought up.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "audio/UiSoundPolicy.h"

namespace uisounds {

struct Stats {
  bool master;        // audioOutEnabled
  bool ui;            // uiSoundsEnabled
  bool armed;         // init() ran and the worker exists
  bool pathOk;        // not quarantined (fault / parked / permanent failure)
  uint8_t workerState;  // 0 = not created, 1 = blocked on the semaphore, 2 = playing
  uint32_t started;   // sounds that reached the speaker path
  uint32_t queued;    // events accepted into the one-deep queue
  uint32_t dropped;   // events refused because the player was busy
  uint32_t skipped;   // events refused by a gate
  uint32_t failed;    // playPcm() returned false
  uint32_t lastCostUs;  // measured cost of the last onEvent() call
  uint32_t traceOn;     // 1 while the per-event trace is switched on
  uint32_t saves;       // SETTINGS writes so far (the double-persist witness)
  uint16_t lastFrames;
  char reason[48];
};

// AUDIO3 — one traced call, as the shell prints it. `ageMs` is how long before
// the dump it happened; that is enough to correlate a burst with a gesture
// without the event path ever reading a clock it would have to log.
struct TraceEntry {
  uint8_t tag;
  uint8_t event;
  uint8_t outcome;
  uint16_t ageMs;
};

// AUDIO3 — the per-call-site counters. Zeroed at boot; nothing clears them, so
// "the Home grid never emitted in this session" is a readable statement.
const Ledger& ledger();

// The last kTraceDepth events, oldest first. `count` is what is valid.
int trace(TraceEntry* out, int maxOut);

// Per-event tracing to the serial log (compiled in at LOG_LEVEL>=1). OFF by
// default: the event path stays log-free unless the operator asks for a trace,
// and even then at most one line per kTraceMinGapMs.
void setTrace(bool on);
bool traceOn();

// Builds the waveforms and starts the worker. Touches NO rail, UART or I2S and
// is safe on the boot path; with both gates off it only reads the two settings.
void init();

// The event path. Never blocks, never allocates, and logs only while an
// operator has switched the trace on (setTrace) -- at most one line per event
// and never more than one per kTraceMinGapMs. `tag` names the calling surface so
// the ledger can attribute a drop; callers should always pass one.
void onEvent(Event e, Tag tag = Tag::Unknown);

// Persist SETTINGS.uiSoundsEnabled and move the cached gate. AUDIO3: this does
// NOT save -- see UiSounds.cpp.
bool setEnabled(bool on);  // does NOT persist

bool enabled();

// Operator/test playback: runs the same gates, but SYNCHRONOUSLY on the caller
// so a shell command can report success. Do not call from the UI task.
bool playNow(Sound s);

Stats stats();

}  // namespace uisounds
