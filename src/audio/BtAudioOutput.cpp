// TTS1 — BH1098G audio output path. See BtAudioOutput.h for the pin sources.
//
// ORDER OF OPERATIONS (vendor metalio_audio.cc:26-27,93-130 + config.h:142-154):
//   P1.4 high -> 700 ms settle -> "AT+RX=2" -> 700 ms -> "AT+MODE=1"
//   -> await "SET MODE 1" (<= 1000 ms) -> I2S TX (slave, 16 kHz, 32-bit slots)
//   -> P0.4 (amp) high.
//   Teardown is the reverse, and the rail is the LAST thing off.
//
// EVERY WAIT HAS A DEADLINE. There is exactly one attempt per power-on; a chip
// that does not answer gets audio disabled for the session (State::Faulted) and
// is not retried until the operator asks again.

#include "audio/BtAudioOutput.h"

#include <Arduino.h>

#include <driver/i2s_std.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "Logging.h"
#include "epd_board_metalio_eink4_plus.h"
#include "platform/StandbyTrims.h"

namespace btaudio {
namespace {

// --- pins: vendor config.h:142-154 @ ad8edf4. Do not change without a source. ---
constexpr int kI2sBclk = 42;
constexpr int kI2sWs = 43;
constexpr int kI2sDout = 44;
constexpr int kUartTx = 39;  // host TX -> BH RX
constexpr int kUartRx = 38;  // host RX <- BH TX
constexpr uart_port_t kUart = UART_NUM_1;
constexpr uint32_t kRate = 16000;

// --- timing: vendor kBootSettleUs / kAckTimeoutMs / the RX->MODE delay -------
constexpr uint32_t kRailSettleMs = 700;
constexpr uint32_t kRxToModeMs = 700;
constexpr uint32_t kAckTimeoutMs = 1000;
constexpr uint32_t kChunkWriteTimeoutMs = 60;  // one 32 ms DMA chunk + slack

// 512 frames = 32 ms at 16 kHz. Stereo 32-bit, mono sample in both slots.
constexpr size_t kFrames = 512;
int32_t g_pcm[kFrames * 2];
int16_t g_mono[kFrames];  // staging for the generated sine (playTone)

// AUDIO1: hard ceiling on one playPcm() call, so a caller cannot hand the DMA a
// buffer that keeps this task for a second. 4000 frames = 250 ms at 16 kHz.
constexpr uint32_t kMaxPcmFrames = 4000;

SemaphoreHandle_t g_mu = nullptr;
bool g_muReady = false;

State g_state = State::Off;
std::atomic<bool> g_gate{false};  // read without the lock by the mid-tone abort check
bool g_railOn = false;
bool g_ampOn = false;
bool g_i2sUp = false;
bool g_handshakeOk = false;
bool g_uartOpen = false;
uint32_t g_powerOns = 0;
uint32_t g_tones = 0;
uint32_t g_writeTimeouts = 0;
uint16_t g_lastRate = 0;
char g_reason[56] = "never powered";

i2s_chan_handle_t g_tx = nullptr;

void setReason(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void setReason(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_reason, sizeof(g_reason), fmt, ap);
  va_end(ap);
}

// Serialises every rail/UART/I2S transition. Taken with a finite timeout by the
// public entry points, so a wedged tone can never hang the shell.
struct Guard {
  bool held;
  explicit Guard(uint32_t ms) : held(g_muReady && xSemaphoreTake(g_mu, pdMS_TO_TICKS(ms)) == pdTRUE) {}
  ~Guard() {
    if (held) xSemaphoreGive(g_mu);
  }
  bool ok() const { return held; }
};

bool uartOpen() {
  if (g_uartOpen) return true;
  const esp_err_t e = uart_driver_install(kUart, 2048, 0, 0, nullptr, 0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    setReason("uart install failed: %s", esp_err_to_name(e));
    return false;
  }
  uart_config_t cfg = {};
  cfg.baud_rate = 115200;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  if (uart_param_config(kUart, &cfg) != ESP_OK ||
      uart_set_pin(kUart, kUartTx, kUartRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    setReason("uart config failed");
    uart_driver_delete(kUart);
    return false;
  }
  uart_flush_input(kUart);
  g_uartOpen = true;
  return true;
}

void uartClose() {
  if (!g_uartOpen) return;
  uart_driver_delete(kUart);
  g_uartOpen = false;
}

void sendLine(const char* line) {
  if (!g_uartOpen) return;
  uart_write_bytes(kUart, line, strlen(line));
  uart_wait_tx_done(kUart, pdMS_TO_TICKS(50));
}

// Bounded read for "SET MODE 1". Line-oriented, tolerant of CR/LF and of other
// chatter (the module can emit RECONNECT lines). Never blocks past the deadline.
bool awaitModeAck(uint32_t timeoutMs) {
  char line[96];
  size_t n = 0;
  const int64_t deadline = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
  while (esp_timer_get_time() < deadline) {
    uint8_t c = 0;
    const int got = uart_read_bytes(kUart, &c, 1, pdMS_TO_TICKS(10));
    if (got != 1) continue;
    if (c == '\r' || c == '\n') {
      line[n] = 0;
      n = 0;
      if (strstr(line, "SET MODE 1") != nullptr) return true;
      if (strstr(line, "ERROR") != nullptr || strstr(line, "FAIL") != nullptr) return false;
      continue;
    }
    if (c >= 32 && c < 127) {
      if (n + 1 < sizeof(line)) line[n++] = char(c);
      n = n < sizeof(line) - 1 ? n : sizeof(line) - 1;  // drop overlong tail
    }
  }
  return false;
}

void i2sTearDown() {
  if (g_tx != nullptr) {
    if (g_i2sUp) i2s_channel_disable(g_tx);
    i2s_del_channel(g_tx);
    g_tx = nullptr;
  }
  g_i2sUp = false;
}

bool i2sBringUp() {
  i2s_chan_config_t chan = {};
  chan.id = I2S_NUM_0;
  chan.role = I2S_ROLE_SLAVE;  // the BH1098G is the clock master (vendor bt_audio_codec.cc:66-69)
  chan.dma_desc_num = 4;
  chan.dma_frame_num = kFrames;
  chan.auto_clear_after_cb = true;
  chan.auto_clear_before_cb = false;
  chan.allow_pd = false;
  chan.intr_priority = 0;
  chan.tx_destination = I2S_DESTINATION_DMA;
  chan.rx_destination = I2S_DESTINATION_DMA;
  esp_err_t e = i2s_new_channel(&chan, &g_tx, nullptr);
  if (e != ESP_OK) {
    setReason("i2s_new_channel: %s", esp_err_to_name(e));
    return false;
  }

  i2s_std_config_t std = {};
  std.clk_cfg.sample_rate_hz = kRate;
  std.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
  std.clk_cfg.ext_clk_freq_hz = 0;
#endif
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  std.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
  std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  std.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
  std.slot_cfg.ws_pol = false;
  std.slot_cfg.bit_shift = true;  // Philips, MSB first, left aligned
#if SOC_I2S_HW_VERSION_2
  std.slot_cfg.left_align = true;
  std.slot_cfg.big_endian = false;
  std.slot_cfg.bit_order_lsb = false;
#endif
  std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std.gpio_cfg.bclk = static_cast<gpio_num_t>(kI2sBclk);
  std.gpio_cfg.ws = static_cast<gpio_num_t>(kI2sWs);
  std.gpio_cfg.dout = static_cast<gpio_num_t>(kI2sDout);
  std.gpio_cfg.din = I2S_GPIO_UNUSED;

  e = i2s_channel_init_std_mode(g_tx, &std);
  if (e == ESP_OK) e = i2s_channel_enable(g_tx);
  if (e != ESP_OK) {
    setReason("i2s init/enable: %s", esp_err_to_name(e));
    i2sTearDown();
    return false;
  }
  g_i2sUp = true;
  return true;
}

// Reverse of powerOn, in the safe order. Caller holds the guard.
void tearDownLocked(const char* why) {
  if (g_ampOn) {
    epd_board_metalio_eink4_plus_pa_set(false);
    g_ampOn = false;
  }
  i2sTearDown();
  uartClose();
  g_handshakeOk = false;
  if (g_railOn) {
    // Never fight the standby park for P1.4: while parked the park owns the
    // rail and will restore it itself. Writing false here is not dangerous,
    // but it would contradict the park's own ledger.
    if (standbytrim::parked()) {
      setReason("%s (rail left to the standby park)", why);
    } else if (!epd_board_metalio_eink4_plus_pa_rail_set(false)) {
      setReason("%s (P1.4 cut FAILED)", why);
      g_state = State::Faulted;
      return;
    }
    g_railOn = false;
  }
  setReason("%s", why);
  if (g_state != State::Faulted) g_state = State::Off;
}

// AUDIO1: the one place a mono block reaches the DMA. Shared by playTone() and
// playPcm() so both have exactly the same chunk size, deadline and fault rule.
// Caller holds the guard and g_state == Ready on entry.
// Returns false when the session was torn down (g_state already set).
bool writeMonoBlockLocked(const int16_t* mono, uint32_t n, int& consecutiveTimeouts) {
  if (n == 0 || n > kFrames) return false;
  // 16-bit sample left-aligned in a 32-bit slot (vendor MetalioPcm.h), both slots.
  for (uint32_t i = 0; i < n; i++) {
    const int32_t v = int32_t(mono[i]) << 16;
    g_pcm[i * 2] = v;
    g_pcm[i * 2 + 1] = v;
  }
  for (;;) {
    size_t written = 0;
    const esp_err_t e =
        i2s_channel_write(g_tx, g_pcm, size_t(n) * 2 * sizeof(int32_t), &written, kChunkWriteTimeoutMs);
    if (e == ESP_OK && written != 0) {
      consecutiveTimeouts = 0;
      return true;
    }
    if (e == ESP_ERR_TIMEOUT || written == 0) consecutiveTimeouts++;
    if (consecutiveTimeouts >= 2) {
      g_writeTimeouts++;
      tearDownLocked("i2s write timed out -- no BCLK from the chip (mute/absent); session disabled");
      g_state = State::Faulted;
      LOG_ERR("AUDIO", "btaudio playback aborted: %s", g_reason);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

void init() {
  // Idempotent: status()/playTone()/powerOn() all call it, and the shell may ask
  // for status before anything else has run. It must never clobber a live state.
  if (g_muReady) return;
  g_mu = xSemaphoreCreateMutex();
  g_muReady = (g_mu != nullptr);
  g_gate.store(SETTINGS.audioOutEnabled != 0);
  g_state = g_gate ? State::Off : State::Disabled;
  setReason(g_gate ? "gate on, idle (rail untouched)" : "disabled by setting");
  LOG_INF("AUDIO", "btaudio init: gate=%d state=%s (no rail access)", int(g_gate), stateName(g_state));
}

const char* stateName(State s) {
  switch (s) {
    case State::Disabled: return "disabled";
    case State::Off: return "off";
    case State::Ready: return "ready";
    case State::Faulted: return "faulted";
  }
  return "?";
}

Status status() {
  if (!g_muReady) init();
  Status s = {};
  s.state = g_state;
  s.gate = g_gate;
  s.railOn = g_railOn;
  s.ampOn = g_ampOn;
  s.i2sUp = g_i2sUp;
  s.handshakeOk = g_handshakeOk;
  s.powerOns = g_powerOns;
  s.tones = g_tones;
  s.writeTimeouts = g_writeTimeouts;
  s.lastRate = g_lastRate;
  snprintf(s.reason, sizeof(s.reason), "%s", g_reason);
  return s;
}

bool gate() { return g_gate.load(); }

bool setGate(bool on) {
  if (!g_muReady) init();
  // Flip the gate FIRST, outside the lock: a tone in flight checks it between
  // 32 ms chunks and aborts itself, which is what releases the lock we are
  // about to want. Waiting for the lock before flipping it would deadlock the
  // disable behind the very playback it is supposed to stop.
  g_gate.store(on);
  SETTINGS.audioOutEnabled = on ? 1 : 0;

  // AUDIO3 (was AUDIO2's S2 defect): NO save and NO wait here.
  //
  //   * Persisting belongs to the caller that owns the setting. SettingsActivity
  //     already saves once per change before calling us, so the save that used
  //     to live here made one toggle tap write the settings file twice -- two SD
  //     writes on the input path for one switch.
  //   * The 1500 ms Guard used to block the UI task waiting for a tone to end.
  //     A TRY-LOCK is enough: if the audio mutex is busy the player is mid-tone
  //     and playPcm()'s own chunk loop already re-reads g_gate after every 32 ms
  //     block (:531) and tears the session down itself. The blocking wait only
  //     ever helped in the one case that is already handled asynchronously.
  //     So the caller now costs a store plus (at worst) one failed try.
  {
    Guard g(0);
    if (!g.ok()) {
      if (!on) g_state = State::Disabled;
      setReason("gate=%d; audio lock busy (player tears down on its next chunk)", int(on));
      LOG_INF("AUDIO", "btaudio gate=%d handed to the player (no wait)", int(on));
      return true;
    }
    if (!on) {
      // The player has already stopped: it saw the gate drop between chunks.
      tearDownLocked("disabled by setting");
      g_state = State::Disabled;  // after the teardown, which settles on Off
    } else if (g_state == State::Disabled) {
      g_state = State::Off;
      setReason("gate on, idle");
    }
  }
  LOG_INF("AUDIO", "btaudio gate=%d state=%s", int(on), stateName(g_state));
  return true;
}

bool powerOn() {
  if (!g_muReady) init();
  Guard g(3000);
  if (!g.ok()) {
    setReason("busy (another audio transition holds the lock)");
    return false;
  }
  if (!g_gate) {
    setReason("gate off");
    return false;
  }
  if (g_state == State::Ready) return true;
  // While the device is parked, P1.4 belongs to the park: raising it here would
  // stack a load on a rail the park just cut and then fight the resume.
  if (standbytrim::parked()) {
    setReason("refused: device is parked (P1.4 owned by the standby path)");
    return false;
  }

  g_state = State::Off;
  const bool railWasOn = g_railOn;

  // 1. rail. P1.4 only -- P0.5 (CAM_SCR) belongs to the panel and is untouched.
  if (!railWasOn) {
    if (!epd_board_metalio_eink4_plus_pa_rail_set(true)) {
      tearDownLocked("P1.4 raise failed (TCA not ready)");
      g_state = State::Faulted;
      return false;
    }
    g_railOn = true;
    g_powerOns++;
    vTaskDelay(pdMS_TO_TICKS(kRailSettleMs));  // vendor kBootSettleUs
  }

  // 2. UART + mode handshake. One attempt, hard deadline.
  if (!uartOpen()) {
    tearDownLocked("UART1 open failed");
    g_state = State::Faulted;
    return false;
  }
  sendLine("AT+RX=2\r\n");
  vTaskDelay(pdMS_TO_TICKS(kRxToModeMs));
  uart_flush_input(kUart);
  sendLine("AT+MODE=1\r\n");
  if (!awaitModeAck(kAckTimeoutMs)) {
    tearDownLocked("no 'SET MODE 1' within 1000 ms (chip absent or mode 1 refused)");
    g_state = State::Faulted;
    return false;
  }
  g_handshakeOk = true;

  // 3. I2S TX at 16 kHz / 32-bit stereo slots, then the amp.
  if (!i2sBringUp()) {
    tearDownLocked("I2S TX bring-up failed");
    g_state = State::Faulted;
    return false;
  }
  epd_board_metalio_eink4_plus_pa_set(true);
  g_ampOn = true;
  g_lastRate = static_cast<uint16_t>(kRate);
  g_state = State::Ready;
  setReason("mode 1 acked, i2s up, amp on");
  LOG_INF("AUDIO", "btaudio ready: rail=%d amp=%d rate=%u", int(g_railOn), int(g_ampOn), unsigned(g_lastRate));
  return true;
}

void powerOff() {
  if (!g_muReady) return;
  Guard g(1500);
  if (!g.ok()) {
    setReason("powerOff skipped: audio lock busy");
    return;
  }
  tearDownLocked("powered down on request");
  if (g_state != State::Faulted) g_state = g_gate ? State::Off : State::Disabled;
}

bool playTone(uint16_t hz, uint16_t ms) {
  if (!g_muReady) init();
  if (hz < 50) hz = 50;
  if (hz > 4000) hz = 4000;
  if (ms == 0) ms = 20;
  if (ms > 1000) ms = 1000;  // bounded: the shell task must not sit here for seconds

  // Pre-check under the lock, release it, then let powerOn() take it itself --
  // a FreeRTOS mutex is NOT recursive.
  {
    Guard g(1200);
    if (!g.ok()) {
      setReason("tone refused: audio busy");
      return false;
    }
    if (!g_gate) {
      setReason("tone refused: gate off");
      return false;
    }
    if (g_state == State::Faulted) {
      setReason("tone refused: session faulted (%s)", g_reason);
      return false;
    }
  }
  if (g_state != State::Ready && !powerOn()) return false;

  Guard g(2500);
  if (!g.ok()) {
    setReason("tone refused: audio busy");
    return false;
  }
  if (g_state != State::Ready || g_tx == nullptr) return false;

  // 16 kHz mono sine, both slots, 16-bit sample left-aligned in a 32-bit slot
  // (the vendor shifts its samples into the top half of the slot: MetalioPcm.h).
  constexpr double kAmp = 7200.0;  // ~22 % of full scale: audible, not startling
  const uint32_t step = uint32_t((uint64_t(hz) << 32) / kRate);
  uint32_t phase = 0;
  const uint32_t totalFrames = (uint32_t(kRate) * ms) / 1000;
  uint32_t done = 0;
  int consecutiveTimeouts = 0;

  while (done < totalFrames) {
    uint32_t n = totalFrames - done;
    if (n > kFrames) n = kFrames;
    for (uint32_t i = 0; i < n; i++) {
      const double t = double(phase) / 4294967296.0;
      g_mono[i] = int16_t(kAmp * sin(2.0 * M_PI * t));
      phase += step;
    }
    if (!writeMonoBlockLocked(g_mono, n, consecutiveTimeouts)) return false;
    done += n;
    if (!g_gate) {
      tearDownLocked("tone aborted: gate turned off mid-play");
      g_state = State::Disabled;
      return false;
    }
  }
  g_tones++;
  setReason("tone %u Hz %u ms played", unsigned(hz), unsigned(ms));
  return true;
}

bool playPcm(const int16_t* mono, uint32_t frames) {
  if (!g_muReady) init();
  if (mono == nullptr || frames == 0) return false;
  if (frames > kMaxPcmFrames) frames = kMaxPcmFrames;  // bounded, never unbounded here

  // AUDIO1: identical gate/lock/prologue to playTone(). Nothing below this line
  // is new policy: the same one-attempt power-on, the same 60 ms chunk deadline,
  // the same fault-on-two-timeouts teardown.
  {
    Guard g(1200);
    if (!g.ok()) {
      setReason("pcm refused: audio busy");
      return false;
    }
    if (!g_gate) {
      setReason("pcm refused: gate off");
      return false;
    }
    if (g_state == State::Faulted) {
      setReason("pcm refused: session faulted (%s)", g_reason);
      return false;
    }
  }
  if (g_state != State::Ready && !powerOn()) return false;

  Guard g(2500);
  if (!g.ok()) {
    setReason("pcm refused: audio busy");
    return false;
  }
  if (g_state != State::Ready || g_tx == nullptr) return false;

  uint32_t done = 0;
  int consecutiveTimeouts = 0;
  while (done < frames) {
    uint32_t n = frames - done;
    if (n > kFrames) n = kFrames;
    if (!writeMonoBlockLocked(mono + done, n, consecutiveTimeouts)) return false;
    done += n;
    if (!g_gate) {
      tearDownLocked("pcm aborted: gate turned off mid-play");
      g_state = State::Disabled;
      return false;
    }
  }
  g_tones++;
  setReason("pcm %u frames played", unsigned(frames));
  return true;
}

}  // namespace btaudio
