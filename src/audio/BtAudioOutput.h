#pragma once

// ---------------------------------------------------------------------------
// TTS1 — the audio OUTPUT path, brought up behind a persisted gate.
//
// WHAT THIS IS
//   The chain on this board is: ESP I2S DOUT -> BH1098G (DAC + clock master)
//   -> NS4150 (amp, expander P0.4) -> speaker. There is NO ESP-side DAC, no
//   buzzer and no PWM tone pin, so every sound the firmware makes has to go
//   through the BH1098G, which must first be put in mode 1 over UART1.
//
// PINS — all taken from the vendor repo, none invented.
//   vendor clone: M:/_HermesWork/crosspoint-fork/_imports/ref/metalio-upstream
//                 @ ad8edf4 ("修正一些过时的描述")
//     main/boards/metalio-e-ink4-plus/config.h:142-154
//       AUDIO_I2S_BCLK GPIO42, AUDIO_I2S_WS GPIO43, AUDIO_I2S_DOUT GPIO44,
//       AUDIO_I2S_DIN GPIO45, 16000 Hz in and out,
//       BT_AUDIO_TX_PIN GPIO39, BT_AUDIO_RX_PIN GPIO38 (host side, 115200 8N1)
//     config.h:71   EPD_IO_PA_EN 4  -> TCA P0.4 (NS4150 CTRL, active high)
//     config.h:100  EPD_IO_BT_PA_PWR 12 -> TCA P1.4 (4G + BT + amp rail)
//   Our tree already owns P1.4 as `epd_board_metalio_eink4_plus_pa_rail_set()`
//   and P0.4 as `epd_board_metalio_eink4_plus_pa_set()`; this module calls only
//   those two, so the TCA shadow stays consistent with the rest of the tree.
//
// WHAT IT IS NOT
//   * Not called from the boot path. With the setting off (the default) no code
//     here ever touches the rail or the UART.
//   * No retry loop, no unbounded wait. Every stage has a deadline; a chip that
//     does not answer disables audio for the session and is not retried.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace btaudio {

enum class State : uint8_t {
  Disabled = 0,  // gate off (setting): the rail has never been touched
  Off,           // gate on, path down (idle = rail low)
  Ready,         // rail up, chip in mode 1, I2S TX enabled
  Faulted        // a bounded stage failed; audio is off for this session
};

struct Status {
  State state;
  bool gate;          // the persisted setting
  bool railOn;        // P1.4 as this module last drove it
  bool ampOn;         // P0.4 as this module last drove it
  bool i2sUp;
  bool handshakeOk;   // "SET MODE 1" seen for the current power cycle
  uint32_t powerOns;  // rail power-on transitions this session
  uint32_t tones;
  uint32_t writeTimeouts;
  uint16_t lastRate;
  char reason[56];
};

// Reads the persisted gate. Does NOT touch the rail, the UART or I2S.
// Safe to call from the boot path; currently the only caller is the shell.
void init();

Status status();

// Move the gate (SETTINGS.audioOutEnabled and the cached atomic). `false` also
// powers the path down, but never waits: if a tone is playing it is the player
// that tears the session down, between chunks. AUDIO3: does NOT persist -- the
// caller that owns the setting saves once (see the definition).
bool setGate(bool on);
bool gate();

// Rail up -> 700 ms settle -> AT+RX=2 -> 700 ms -> AT+MODE=1 -> wait ack
// (<=1000 ms) -> I2S TX up -> amp on.   Bounded at ~2.4 s worst case.
// Returns false and leaves the path OFF (rail low) on any failure.
bool powerOn();

// Amp off -> I2S down -> rail low. Idempotent, safe from any state.
// `audio disable` and every failure path end here.
void powerOff();

// Generate and play a sine. Lazy power-on. Returns false if the gate is off,
// the path faulted, another tone is playing, or a DMA write timed out.
bool playTone(uint16_t hz, uint16_t ms);

// AUDIO1: play a precomputed 16 kHz MONO int16 buffer through the same path.
// Same gates, same lock, same one-attempt power-on, same chunk deadline and
// fault rule as playTone() -- it is the same writer, just a caller-supplied
// waveform instead of a generated sine. `frames` is capped at 4000 (250 ms).
// Blocking for the caller (that is why UiSounds runs it on its own task).
bool playPcm(const int16_t* mono, uint32_t frames);

const char* stateName(State s);

}  // namespace btaudio
