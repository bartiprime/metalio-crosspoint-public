#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>
#include <cstdarg>
#include <cstring>

#include "HalGPIO.h"

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <MetalioEInk4PlusBoard.h>
#include <Wire.h>
#endif

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO || FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <HalFrontlight.h>   // HalFrontlight itself is board-agnostic; the manager is inert where absent
#endif

#if FREEINK_DEVICE_PICCO
#include <driver/gpio.h>
#endif

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  // Board charger setup (Picco SGM41562: stock charge voltage and limits); a
  // no-op on boards without a configurable charger.
  if (BatteryMonitor::configureCharger()) LOG_INF("PWR", "Charger configured");
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Held across the check and the switch: a Lock taken in between would find full speed, do
  // nothing, and then run at LOW_POWER_FREQ until the next key press.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  // LOCK1: a live Lock is a hold, whatever task took it -- the old single slot tested here as
  // `currentLockMode != None` and is now `speedLockCount_ != 0`. Same edge, same frequency.
  const bool speedLocked = speedLockCount_ != 0;

  // r99.5 DRIVE5: a HELD need outranks the caller's opinion, and it is tested inside the same
  // critical section that applies the switch -- so a caller whose `enabled=true` decision was
  // made before the hold existed (exactly the r99.4 race: the main loop decided in the unarmed
  // state, the activity armed one call later) simply cannot act on it. The device also cannot
  // be left at low speed by a hold that arrives while it is low: the hold's acquire() calls
  // back into this function to restore normal frequency immediately.
  if (fullSpeedHolds_ > 0) enabled = false;

  if (!speedLocked && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(idleFreqMhz_)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", idleFreqMhz_);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = true;
    lastTransition_.atMs = millis() ? millis() : 1u;  // 0 means "none since boot"
    lastTransition_.toLowPower = true;

  } else if ((!enabled || speedLocked) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = false;
    lastTransition_.atMs = millis() ? millis() : 1u;
    lastTransition_.toLowPower = false;
  }

  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

// Step 2.5 item 9: the idle frequency is a setting, and this is the only writer of
// it. Read and written under the same mutex setPowerSaving() applies the switch
// with, so a change can never be split from the transition it belongs to. If the
// device is ALREADY idle the new value is applied here -- otherwise the operator
// would pick a step and see nothing until the next idle edge, which is exactly the
// "the setting did nothing" shape this work exists to avoid. The restore path is
// untouched: leaving idle is still setCpuFrequencyMhz(normalFreq).
void HalPowerManager::setIdleCpuFreqMhz(const int mhz) {
  // The four steps are the setting's 80/160/240/320. Anything else is a caller bug,
  // not a value to clamp: refuse it and keep the frequency that is running.
  if (mhz < 10 || mhz > 320) {
    LOG_ERR("PWR", "idle cpu: refusing %d MHz (outside the 10..320 MHz range)", mhz);
    return;
  }
  if (modeMutex == nullptr) {
    idleFreqMhz_ = mhz;  // before begin(): there is nothing to switch yet
    LOG_INF("PWR", "idle cpu: set=%d MHz (applies when idle)", mhz);
    return;
  }

  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const int previous = idleFreqMhz_;
  idleFreqMhz_ = mhz;
  if (isLowPower && mhz != previous) {
    if (setCpuFrequencyMhz(mhz)) {
      lastTransition_.atMs = millis() ? millis() : 1u;
      lastTransition_.toLowPower = true;
    } else {
      idleFreqMhz_ = previous;  // the write failed: do not claim the new value
      LOG_ERR("PWR", "idle cpu: setCpuFrequencyMhz(%d) failed, keeping %d MHz", mhz, previous);
    }
  }
  const bool idleActive = isLowPower;
  xSemaphoreGive(modeMutex);

  // One line at boot and one per change. The frequency reported as `applied` is the
  // one the CPU is actually running at now (getCpuFrequencyMhz), not the request.
  LOG_INF("PWR", "idle cpu: set=%d MHz applied=%d MHz (idle=%d MHz, low power %s)", mhz, getCpuFrequencyMhz(),
          idleFreqMhz_, idleActive ? "active" : "not yet");
}

// r99.5 DRIVE5: see the class comment in the header. The refcount and the switch share one
// mutex, so "who holds full speed" can never be read across a frequency change.
void HalPowerManager::FullSpeedHold::acquire(const char* reason) {
  if (held_) return;
  if (powerManager.modeMutex == nullptr) return;  // called before begin(): nothing to protect
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  ++powerManager.fullSpeedHolds_;
  const int holds = powerManager.fullSpeedHolds_;
  xSemaphoreGive(powerManager.modeMutex);
  held_ = true;
  LOG_INF("PWR", "full-speed hold TAKEN (%s) holds=%d -- no frequency change may be applied until released",
          reason != nullptr ? reason : "?", holds);
  // Leave low power NOW, not on the next idle decision: the caller's very next step may already
  // be the one that needs the PLL (routing the pads, starting the gadget).
  powerManager.setPowerSaving(false);
}

void HalPowerManager::FullSpeedHold::release() {
  if (!held_) return;
  held_ = false;
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.fullSpeedHolds_ > 0) --powerManager.fullSpeedHolds_;
  const int holds = powerManager.fullSpeedHolds_;
  xSemaphoreGive(powerManager.modeMutex);
  // No frequency change on release: the next setPowerSaving() decision owns that, and it may
  // only drop the clock when the caller actually asked for it.
  LOG_INF("PWR", "full-speed hold RELEASED holds=%d", holds);
}

bool HalPowerManager::fullSpeedHeld() const { return fullSpeedHoldCount() > 0; }

int HalPowerManager::fullSpeedHoldCount() const {
  if (modeMutex == nullptr) return 0;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const int holds = fullSpeedHolds_;
  xSemaphoreGive(modeMutex);
  return holds;
}

// ---------------------------------------------------------------------------
// r96 read-only accessors for the `pm` shell readout. No behaviour change: they
// report exactly what setPowerSaving() holds.
// ---------------------------------------------------------------------------
bool HalPowerManager::normalSpeedLockHeld() const {
  if (modeMutex == nullptr) return false;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool held = speedLockCount_ != 0;
  xSemaphoreGive(modeMutex);
  return held;
}

HalPowerManager::Transition HalPowerManager::lastTransition() const { return lastTransition_; }

void HalPowerManager::startDeepSleep(HalGPIO& gpio, const bool wakeOnHeldButton) const {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    // This board wakes from EXT1 ANY_LOW on the expander's INT (GPIO2), and the accelerometer's INT1
    // is wired onto the same line. If either still holds it low when the chip sleeps, the wake is
    // immediate and the device becomes a boot loop that repaints the panel every cycle. The vendor
    // reference (power_hw: WaitIntLineHigh -> SleepAbort) refuses to sleep in that state; so do we.
    // Checked before hibernate/frontlight-off so an aborted sleep leaves the device usable.
    if (!freeink::metalio4plus::waitForInterruptLineIdle(300)) {
      LOG_INF("PWR", "not sleeping: expander/accel INT (GPIO%d) still low", freeink::metalio4plus::IO_EXPANDER_INT_GPIO);
      return;
    }
  }
#endif
#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO || FREEINK_DEVICE_METALIO_EINK4_PLUS
  // The frontlight driver and touch share I2C; turn the light off before touch
  // releases the bus. The Plus is included: its touch controller is an FT6336U, which now has a
  // hibernate path, and leaving the frontlight driver enabled through sleep served no purpose.
  Frontlight.setOn(false);
  gpio.prepareForDeepSleep();
#endif
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif
#if FREEINK_DEVICE_METALIO_EINK4
  // Deep sleep leaves the board, 4G modem and audio module powered, so cut
  // power instead; the button cold-boots it. Returns only on USB power.
  freeink::metalio::powerOff();
#endif

  if (wakeOnHeldButton) {
    freeink::PowerManager::armPowerButtonWakeup();
    freeink::PowerManager::deepSleep();
  }
  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#if FREEINK_DEVICE_PICCO
// ponytail: 30 s poll; the SGM41562 status is I2C-only, so USB unplug and
// charge progress are only seen when the timer fires.
static constexpr uint64_t USB_HOLD_POLL_US = 30ULL * 1000 * 1000;

bool HalPowerManager::holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const {
  if (!gpio.isUsbConnected()) return false;
  LOG_DBG("PWR", "USB power present, holding in light sleep");
  const auto powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // The press that requested sleep may still be down; it must not end the hold.
  while (digitalRead(powerPin) == LOW) delay(10);  // Picco power button is active-low
  const BatteryMonitor battery;
  int lastCharging = -1;
  int lastPercent = -1;
  while (gpio.isUsbConnected()) {
    const bool charging = battery.isCharging();
    const uint16_t percent = getBatteryPercentage();
    if (static_cast<int>(charging) != lastCharging || percent != lastPercent) {
      lastCharging = charging;
      lastPercent = percent;
      onStatus(charging, percent);
    }
    esp_sleep_enable_timer_wakeup(USB_HOLD_POLL_US);
    gpio_wakeup_enable(powerPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(powerPin);
    // gpio_wakeup_disable() keeps the LOW_LEVEL type, and a later pinMode() would
    // enable it as an interrupt that storms while the button is held.
    gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (digitalRead(powerPin) == LOW) return true;
  }
  return false;
}
#endif

// Charge state as the hardware reports it, not as the USB-detect line implies:
// a full battery stops charging while the cable is still attached, and a board
// with no USB-detect line can only learn about the cable through this.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
// ---------------------------------------------------------------------------
// r97.1 BATT3 -- CX25601N charge status, board-local.
//
// WHY THIS EXISTS. BatteryMonitor (freeink-sdk) reads charge state from
// "the charger IC at batteryGauge.chargerAddr, CHRG_STAT in REG0x0B[4:3]", which
// is the BQ25896 map. On THIS board 0x6B is a CX25601N: 0x0B there is IOTG_HI
// (an OTG current byte, vendor cx25601n.cc:26), so the decode is meaningless and
// the flag reads 0 forever -- measured on the device as
//   batt: gauge 0x55 chg 0x6B | current +259 mA (into battery) | flag 0 not charging
//   1 chg 0x0B=0x03 CHRG_STAT[4:3]=0 NOT charging
// while 259 mA flowed into the pack. The SDK is a pinned submodule, so the
// correction lives here instead: probe the part once, cache the IDENTIFICATION
// (not the status), and read the real status register per poll.
//
// Register facts, from the vendor tree (M:/_HermesWork/ref/metalio-upstream):
//   cx25601n.h:15   CX25601N_I2C_ADDR 0x6B
//   cx25601n.cc:38  REG_STATUS1 = 0x1E
//   cx25601n.cc:643 cx25601n_get_chrg_stat(): read_bits(REG_STATUS1, 0x03, 3)
//   cx25601n.h:41-44 0 = not charging/terminated, 1 = CC, 2 = CV, 3 = top-off
//   cx25601n.cc:37  REG_PART_INFO = 0x38 (the vendor's own init probe)
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t kCx25601nAddr = 0x6B;
constexpr uint8_t kCx25601nStatus1 = 0x1E;  // status: CHG_STAT[4:3], VBUS_STAT[2:0]
constexpr uint8_t kCx25601nPartInfo = 0x38; // part identification

// 0 = never probed, 1 = CX25601N, 2 = answered but not as a CX25601N.
uint8_t g_cxIdent = 0;

// BUS INVARIANT: ONE shared I2C bus, the same wire the TCA9555 expander lives on;
// the expander's failed read is what caused the power-key glitch. This helper is
// one of four accessor sets on that bus (here, ChargerCurrent, UsbShell, and
// MetalioEInk4PlusBoard.h, where the canonical note lives). B-P2-4: it now takes
// the board's MetalioBusGuard, the same object and the same default timeout the
// board helpers use, so a transaction here can no longer interleave with
// expander/EPD traffic. Lock order is always "bus guard only, never bus-then-panel
// and never panel-then-bus": nothing below this file's reads takes another lock.
// A guard that could not be acquired is reported as a FAILED READ, never as an
// unguarded transfer -- the SoC gate then rejects the sample and keeps the last
// good value, which is the safe direction.
bool cxRead8(const uint8_t addr, const uint8_t reg, uint8_t& out) {
  freeink::metalio4plus::MetalioBusGuard bus;
  if (!bus) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) return false;
  out = static_cast<uint8_t>(Wire.read());
  return true;
}

bool isThisBoard() {
  // Board identity only. The address test that used to be here (`chargerAddr == 0x6B`) is gone
  // because r98.1 zeroes chargerAddr on this board on purpose: leaving it set is what let the SDK
  // read BQ25896's REG0x0B on a CX25601N. The CX25601N still lives at 0x6B and is read through
  // kCx25601nAddr below, which is a constant and cannot be zeroed by accident.
  return BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus;
}

// Identify the part ONCE, and cache only a positive answer (B-P1-6: a cached NAK is what turned one
// unlucky probe into a whole session on the wrong register map). The identification heuristic is
// "REG0x1E answers at 0x6B and decodes as a CX25601N status" (CHG_STAT is a
// 2-bit field, so every value 0..3 is a valid CX25601N status); PART_INFO(0x38)
// is read alongside it because the vendor's own init probe uses that register,
// and it is reported in the boot line so a wrong identification is visible
// rather than silent. When the probe fails we fall through to the SDK's
// BQ25896 path unchanged, which is also the right answer for every other board
// in the fork.
bool cxIdentify() {
  if (g_cxIdent != 0) return g_cxIdent == 1;
  // B-P1-6: a FAILED probe used to be cached as "not this part" for the whole session, so one NAK
  // at boot -- a charger still waking, a bus momentarily busy -- pinned every later read to the
  // BQ25896 register map until the next reboot (and the charging icon with it). Only a SUCCESS is
  // cached on the first answer now; a failure is re-probed on a bounded ladder and only then
  // cached as absent. Worst case is 2 extra probes and ~500 ms, once per boot.
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (attempt != 0) delay(250);
    uint8_t status1 = 0;
    if (!cxRead8(kCx25601nAddr, kCx25601nStatus1, status1)) continue;
    uint8_t part = 0;
    const bool partOk = cxRead8(kCx25601nAddr, kCx25601nPartInfo, part);
    g_cxIdent = 1;
    // One line, at boot, greppable as `[PWR] charger:`.
    LOG_INF("PWR", "charger: CX25601N @0x%02X (REG0x1E=0x%02X chg_stat=%u, PART0x38=0x%02X %s)",
            kCx25601nAddr, status1, static_cast<unsigned>((status1 >> 3) & 0x03), part,
            partOk ? "ok" : "read failed");
    return true;
  }
  g_cxIdent = 2;
  LOG_INF("PWR", "charger: 0x6B REG0x1E did not answer in 3 probes -> BQ25896 path (BatteryMonitor)");
  return false;
}

// True when this board's CX25601N answered AND the status read succeeded; the
// flag is then CHG_STAT != 0. `used` says whether the answer is CX-derived, so
// the caller can fall back to the SDK chain on any failure.
bool cxCharging(bool& used) {
  used = false;
  if (!isThisBoard() || !cxIdentify()) return false;
  uint8_t status1 = 0;
  if (!cxRead8(kCx25601nAddr, kCx25601nStatus1, status1)) return false;
  used = true;
  return ((status1 >> 3) & 0x03) != 0;  // 1 CC, 2 CV, 3 top-off; 0 = terminated
}
}  // namespace
#endif

bool HalPowerManager::isCharging() const {
  refreshChargeState(millis());
  return _chargingCached;
}

// r163 E3: the DISPLAYED flag. Same raw verdict, suppressed while the pack is already
// full -- see the long note on the declaration in the header. Deliberately a separate
// function and not a change to isCharging(): the stats layer must keep seeing the raw
// answer through the CV tail.
bool HalPowerManager::isChargingDisplayed() const {
  refreshChargeState(millis());
  return gaugestate::displayCharging(_chargeFlag.raw(), getBatteryPercentage());
}

// r97.1 BATT3: the missing timer behind both caches. See HalPowerManager.h.
void HalPowerManager::pollBattery() const {
  (void)getBatteryPercentage();
  (void)isCharging();
}

HalPowerManager::ChargerDiag HalPowerManager::chargerDiag() const {
  ChargerDiag d;
  d.chargerAddr = BoardConfig::ACTIVE.batteryGauge.chargerAddr;
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (!isThisBoard()) return d;
  d.cxIdentified = cxIdentify();
  if (!d.cxIdentified) return d;
  uint8_t status1 = 0, part = 0;
  d.statusRead = cxRead8(kCx25601nAddr, kCx25601nStatus1, status1);
  if (d.statusRead) {
    d.reg1E = status1;
    d.chgStat = (status1 >> 3) & 0x03;
    d.charging = d.chgStat != 0;
  }
  d.partRead = cxRead8(kCx25601nAddr, kCx25601nPartInfo, part);
  d.partInfo = part;
#endif
  return d;
}

// r97 BATT2: the single switch point described in the header. Today it reads the
// same cached flag the status line does; when BATT1's evidence says that flag is
// wrong, this body changes and nothing else in the stats layer does.
bool HalPowerManager::chargingForStats() const { return isCharging(); }

// r97: the `batt` readout's view of the cache above. No I/O, no lock: every field
// is a plain member written by the task that also calls this command (the main
// task, through the shell's mailbox), so a torn read is not possible in practice
// and would be harmless anyway.
HalPowerManager::BatteryDiag HalPowerManager::batteryDiag() const {
  BatteryDiag d;
  d.socLastOkMs = _socLastOkMs;
  d.socLastPollMs = _socLastPollMs;
  d.socFails = _socFails;
  d.socRejects = _socRejects;
  d.socStale = _socStale;
  d.socEverRead = _socLastOkMs != 0;
  d.socCached = static_cast<uint16_t>(_batteryCachedPercent);
  d.socRaw = _socRaw;
  memcpy(d.socWhy, _socWhy, sizeof(d.socWhy));
  d.chargingLastPollMs = _chargingLastPollMs;
  d.chargingCached = _chargingCached;
  // r163 E2: the confirmation's own state, so `batt` can show a latched 100 % and the
  // hold behind it (a 100 % with no hold and no confirm is the display, not the evidence).
  d.fullConfirmed = _fullCharge.confirmed();
  d.fullHolding = _fullCharge.holding();
  d.fullHeldMs = _fullCharge.heldMs(millis());
  d.gaugeMv = _gaugeMv;
  d.gaugeMvOk = _gaugeMvOk;
  d.socSamples = _socMachine.samplesInFilter();
  d.socAverage = _socMachine.filterAverage();
  d.socSettling = _socMachine.settling();
  d.socRecalPending = _socMachine.recalibrationPending();
  d.uptimeMs = millis();
  return d;
}

// ---------------------------------------------------------------------------
// r98.1 D2 -- SoC VALIDITY GATE.
//
// The SDK's BQ27220 path ends in `out = soc > 100 ? 100 : soc` (BatteryMonitor.cpp:368).
// A garbage 0x2C read is therefore PROMOTED to 100%: the one failure mode where a
// broken gauge looks like a full battery. The ADC path in the same file already
// refuses to do this (0 mV -> 0%, not 100%), so the discipline exists; it just never
// reached the I2C-gauge path. freeink-sdk is a pinned submodule, so the gate lives
// here, on the board that has the gauge, and the SDK path is simply not used for it.
//
// 0x2C is trusted only when all four of these hold. They are the cheapest facts that
// separate "a number" from "a reading":
//   1. 0x3A OperationStatus and 0x0A BatteryStatus both answer and are not 0xFFFF:
//      a NAK'd or stuck-high bus returns all ones, and a BQ27220 can never report
//      SEC=3 + everything else set.
//   2. 0x2C < 100 is not required, but 0x2C <= 100 is: a value above 100 is not a
//      percentage, so it is REJECTED rather than clamped (that clamp is the bug).
//   3. 0x08 Voltage >= 2500 mV. A pack that reads below that is not a pack the gauge
//      can describe, and an all-zero 0x08 means the same failed transaction that
//      produced the bogus 0x2C.
//   4. Consistency between the two: >=97% needs >=3950 mV, and 0% needs <=3700 mV.
//      Voltage cannot resolve a Li-ion pack finely, but it does rule out "100%" on a
//      3.6 V pack -- which is the exact shape of the failure this gate exists for.
// ---------------------------------------------------------------------------
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
namespace {
constexpr uint8_t kBq27220RegVoltage = 0x08;
constexpr uint8_t kBq27220RegStatus = 0x0A;
constexpr uint8_t kBq27220RegCurrent = 0x0C;  // Current(), signed LE mA, + = into the pack
constexpr uint8_t kBq27220RegSoc = 0x2C;
constexpr uint8_t kBq27220RegSocHi = 0x2D;  // NOT part of StateOfCharge -- see readSocChecked()
constexpr uint8_t kBq27220RegCapacity = 0x10;  // RemainingCapacity(), u16 mAh (coulomb count)
constexpr uint8_t kBq27220RegOpStatus = 0x3A;

bool bqRead16(const uint8_t addr, const uint8_t reg, uint16_t& out) {
  // B-P2-4: same bus guard and same order as cxRead8/the board helpers. A read that cannot take
  // the bus is a failed read, so the SoC gate below rejects it instead of trusting a torn word.
  freeink::metalio4plus::MetalioBusGuard bus;
  if (!bus) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) != 2) return false;
  const uint8_t lo = static_cast<uint8_t>(Wire.read());
  const uint8_t hi = static_cast<uint8_t>(Wire.read());
  out = static_cast<uint16_t>(lo) | static_cast<uint16_t>(hi << 8);
  return true;
}

// One byte, one transaction. This is the correct width for 0x2C.
bool bqRead8(const uint8_t addr, const uint8_t reg, uint8_t& out) {
  // B-P2-4: guarded like bqRead16. See the BUS INVARIANT note above cxRead8().
  freeink::metalio4plus::MetalioBusGuard bus;
  if (!bus) return false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) return false;
  out = static_cast<uint8_t>(Wire.read());
  return true;
}

// Bounded printf-style append, so a refusal carries the FACTS that refused it into `batt`.
struct Why {
  char* buf;
  size_t cap;
  size_t n = 0;
  void add(const char* fmt, ...) {
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (w <= 0) return;
    n += static_cast<size_t>(w);
    if (n >= cap) n = cap - 1;
    buf[n] = '\0';
  }
};
}  // namespace

// r163 E3 -- ONE WINDOWED POLL OF THE CHARGE STATE, shared by every caller.
//
// The vendor's truth table (`docs/battery-soc-and-charge.md` §4.2) is arithmetic and
// lives in src/util/BatteryGaugeState.h; this is only its sample source. Order matters:
//
//   1. the GAUGE's Current() (0x0C) -- the measurement the table is written against.
//      `> +5 mA` charging, `< -5 mA` discharging, and +-5 mA is a DEADBAND in which the
//      previous verdict stands. That deadband is the whole point: Current() is whole-mA
//      quantised and wanders a count or two on an idle pack, so a bare `> 0` flips the
//      bolt several times a second.
//   2. only if that read FAILED, the charger IC's CHG_STAT (CX25601N REG0x1E) -- the
//      r97.1 path. It can say "charging"; it cannot say which way the current flows, so
//      "not charging" leaves the verdict Unknown rather than claiming a discharge.
//   3. only if that is unavailable too, the SDK's own isCharging() (the /STAT pin or a
//      BQ25896 on other boards).
//
// A failed read never becomes "not charging" by itself: it leaves the last verdict in
// place, and _chargingLastPollMs is still advanced, so a dead bus costs one window, not a
// flicker.
// ---------------------------------------------------------------------------
void HalPowerManager::refreshChargeState(const unsigned long now) const {
  if (_chargingLastPollMs != 0 && (now - _chargingLastPollMs) < BATTERY_POLL_MS) return;
  _chargingLastPollMs = now;

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Bq27220 && g.gaugeAddr != 0 &&
      BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    uint16_t raw = 0;
    if (bqRead16(g.gaugeAddr, kBq27220RegCurrent, raw)) {
      _chargeFlag.update(true, static_cast<int16_t>(raw));
      _chargingCached = _chargeFlag.charging();
      return;
    }
    bool fromCx = false;
    const bool cxChargingFlag = cxCharging(fromCx);
    if (fromCx) {
      _chargeFlag.set(cxChargingFlag ? gaugestate::Charge::Charging : gaugestate::Charge::Unknown);
      _chargingCached = _chargeFlag.charging();
      return;
    }
  }
#endif

  static const BatteryMonitor battery;
  _chargingCached = battery.isCharging();
  _chargeFlag.set(_chargingCached ? gaugestate::Charge::Charging : gaugestate::Charge::Unknown);
}

// r163 E2 -- the full-charge confirmation's sample source.
//
// One 0x08 read per poll into FullCharge (src/util/BatteryGaugeState.h): >= 4300 mV held
// 30 s confirms a full pack, <= 4280 mV releases it, and a failed read or a >2 s gap
// resets the hold. It runs BEFORE the SoC verdict and independently of it, because
// voltage is the evidence for "full" -- a gauge whose 0x2C read is being refused can still
// say exactly how many millivolts the pack is at.
void HalPowerManager::feedFullCharge(const unsigned long now) const {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Bq27220 && g.gaugeAddr != 0 &&
      BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    uint16_t mv = 0;
    _gaugeMvOk = bqRead16(g.gaugeAddr, kBq27220RegVoltage, mv);
    _gaugeMv = _gaugeMvOk ? mv : 0;
    _fullCharge.sample(now, _gaugeMvOk, mv);
    return;
  }
#endif
  (void)now;
}

// r163 E2 -- the standby resume hook. Called once per resume, after the rails and the I2C
// bus are back (main.cpp, next to wakerecResume()).
void HalPowerManager::notifyStandbyWake() {
  const unsigned long now = millis();
  _fullCharge.wake(now);
  _socMachine.wake(now);
  // The displayed SoC is held, not zeroed, across the settle: the stale rule below would
  // otherwise report 0 % for the first 3 s after every wake (the >30 s "last good" clock
  // is still running from before the sleep).
  _socLastOkMs = now;
  _socStale = false;
  // Force the next read to be a real one rather than the pre-sleep cache, so the settle
  // starts from a sample taken after the wake.
  _batteryLastPollMs = 0;
  _chargingLastPollMs = 0;
}

// r98.3 -- SoC PLAUSIBILITY GATE (the D2 defect, done properly).
//
// WHAT WAS WRONG: `soc 65380% [0x2C ok]` is 0xFF64. 0x2C StateOfCharge is ONE byte, but both the
// SDK path (BatteryMonitor.cpp:367 readReg16(0x2C) -> `out = soc > 100 ? 100 : soc`) and this
// file's r98.1 gate read it as a little-endian u16. The high half comes from 0x2D, which is not
// part of the register and answers 0xFF, so a perfectly good 100% (low byte 0x64) became 65380 --
// and the SDK's clamp then PRESENTED that failure as a full battery, the one direction a gauge
// must never lie in. The gate below rejects; it never clamps.
//
// The value accepted is a 1-BYTE read of 0x2C, taken twice. 0x2D is still sampled, but only as
// evidence: it is what a 16-bit read would have picked up, and `batt` prints it. It is deliberately
// NOT a rejection criterion on its own -- 0x2D is not part of StateOfCharge, so refusing every
// reading whose 0x2D is non-zero would turn a working gauge into a permanent 0%.
//
// Accepted only when:
//   1. two consecutive 1-byte reads of 0x2C agree -- a torn/transient read cannot pass;
//   2. the value is <= 100 -- a value above 100 is not a percentage;
//   3. 0x0A and 0x3A answer and are not 0xFFFF (a dead bus returns all ones);
//   4. 0x08 Voltage >= 2500 mV, and it is consistent with the SoC (>=97% needs >=3950 mV, 0% needs
//      <=3700 mV) -- this is what rules out "100%" on a 3.6 V pack.
// On any failure the caller keeps the last good value and `_socFails`/`BATTERY_STALE_MS` decide
// when the UI finally reports 0, so one bad read never blinks the icon.
HalPowerManager::SocCheck HalPowerManager::readSocChecked(const uint8_t addr) {
  SocCheck r;
  Why why{r.why, sizeof(r.why)};
  if (addr == 0) {
    why.add("no I2C gauge on this board profile");
    return r;
  }

  if (!bqRead8(addr, kBq27220RegSoc, r.soc1)) {
    why.add("0x2C read failed (I2C NAK): nothing answered");
    return r;
  }
  r.answered = true;
  if (!bqRead8(addr, kBq27220RegSoc, r.soc2)) {
    why.add("second 0x2C read failed (I2C NAK); first was 0x%02X, so it cannot be corroborated", r.soc1);
    return r;
  }
  if (!bqRead8(addr, kBq27220RegSocHi, r.spare)) r.spare = 0;
  r.raw = static_cast<uint16_t>((static_cast<uint16_t>(r.spare) << 8) | r.soc1);

  bool bad = false;
  if (r.soc1 != r.soc2) {
    why.add("two consecutive 0x2C reads disagree (0x%02X vs 0x%02X), so neither is a reading", r.soc1, r.soc2);
    bad = true;
  }
  if (r.soc1 > 100 || r.soc2 > 100) {
    why.add("%svalue %u > 100: not a percentage (REJECTED, not clamped)",
            bad ? "; " : "", static_cast<unsigned>(r.soc1 > r.soc2 ? r.soc1 : r.soc2));
    bad = true;
  }
  if (bad) return r;

  uint16_t status = 0, opStatus = 0, mv = 0;
  if (!bqRead16(addr, kBq27220RegStatus, status)) {
    why.add("0x0A read failed: gauge status unavailable");
    return r;
  }
  if (!bqRead16(addr, kBq27220RegOpStatus, opStatus)) {
    why.add("0x3A read failed: operation status unavailable");
    return r;
  }
  if (!bqRead16(addr, kBq27220RegVoltage, mv)) {
    why.add("0x08 read failed: voltage unavailable, SoC cannot be corroborated");
    return r;
  }
  if (status == 0xFFFF || opStatus == 0xFFFF) {
    why.add("0x0A/0x3A read all ones: no real gauge behind this address");
    return r;
  }
  if (mv < 2500) {
    why.add("%u%% on %u mV: below 2500 mV the gauge cannot describe the pack", r.soc1, mv);
    return r;
  }
  if (r.soc1 >= 97 && mv < 3950) {
    why.add("%u%% on only %u mV: voltage contradicts a full pack", r.soc1, mv);
    return r;
  }
  if (r.soc1 == 0 && mv > 3700) {
    why.add("0%% on %u mV: voltage contradicts an empty pack", mv);
    return r;
  }

  r.soc = r.soc1;
  r.ok = true;
  r.why[0] = '\0';
  return r;
}

// r98.3 -- RemainingCapacity (0x10), the coulomb count, read the same disciplined way. Unlike
// 0x2C this IS a 16-bit register, so the word width is right; what it needs is corroboration. The
// standby A/B differences are single mAh, so a read that cannot be reproduced is worse than no
// read: it would be reported as consumption.
HalPowerManager::CapCheck HalPowerManager::readRemainingCapacityChecked(const uint8_t addr) {
  CapCheck r;
  Why why{r.why, sizeof(r.why)};
  if (addr == 0) {
    why.add("no I2C gauge on this board profile");
    return r;
  }
  if (!bqRead16(addr, kBq27220RegCapacity, r.mah)) {
    why.add("0x10 read failed (I2C NAK)");
    return r;
  }
  r.answered = true;
  if (!bqRead16(addr, kBq27220RegCapacity, r.mah2)) {
    why.add("second 0x10 read failed (I2C NAK); first was %u mAh", r.mah);
    return r;
  }
  if (r.mah != r.mah2) {
    why.add("two 0x10 reads disagree (%u vs %u mAh)", r.mah, r.mah2);
    return r;
  }
  if (r.mah > 20000) {
    why.add("%u mAh is not a coulomb count for this pack", r.mah);
    return r;
  }
  r.ok = true;
  r.why[0] = '\0';
  return r;
}
#else
// Other boards: the BQ27220 gate and the validated coulomb read are Metalio-profile code (the
// gauge and its 1-byte 0x2C quirk are specific to it). Keep the symbols defined so `batt` and the
// shared callers link everywhere, and say plainly that this is not a reading.
HalPowerManager::SocCheck HalPowerManager::readSocChecked(const uint8_t) {
  SocCheck r;
  snprintf(r.why, sizeof(r.why), "SoC gate compiled for the Metalio profile only");
  return r;
}
HalPowerManager::CapCheck HalPowerManager::readRemainingCapacityChecked(const uint8_t) {
  CapCheck r;
  snprintf(r.why, sizeof(r.why), "validated 0x10 reads compiled for the Metalio profile only");
  return r;
}
#endif  // FREEINK_DEVICE_METALIO_EINK4_PLUS

// One place that decides whether the percentage the UI sees came from a reading worth
// trusting. Gauge boards on the Metalio profile go through the gate above (a refused
// reading counts as a failure, exactly like a NAK); every other board and every ADC
// board keeps the SDK behaviour unchanged. `last`, when supplied, receives the whole
// gate verdict so `batt` can print the raw word and the reason it was refused.
static bool readGaugePercentChecked(const BatteryMonitor& battery, uint16_t& out, uint32_t& rejects,
                                    HalPowerManager::SocCheck* last) {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeType == BoardConfig::GaugeType::Bq27220 && g.gaugeAddr != 0 &&
      BoardConfig::ACTIVE.board == BoardConfig::Board::MetalioEInk4Plus) {
    const HalPowerManager::SocCheck c = HalPowerManager::readSocChecked(g.gaugeAddr);
    if (last != nullptr) *last = c;
    if (!c.ok) {
      if (c.answered) ++rejects;
      static bool warned = false;
      if (!warned) {
        warned = true;
        LOG_ERR("PWR", "gauge: SoC reading REFUSED (%s) -> last good value kept, never clamped to 100%%",
                c.why);
      }
      return false;
    }
    out = c.soc;
    return true;
  }
#endif
  (void)rejects;
  (void)last;
  return battery.readPercentageChecked(out);
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    _socLastPollMs = now;
    // r163 E3/E1: the charge verdict first -- it is the DIRECTION the SoC machine is
    // allowed to move in, so it must be as fresh as the sample it governs (one windowed
    // 0x0C read, no extra bus traffic of its own).
    refreshChargeState(now);
    // r163 E2: the confirmation's sample first, so a refused 0x2C still advances (or
    // resets) it -- millivolts are the evidence for "full", not the percentage register.
    feedFullCharge(now);
    uint16_t percent = 0;
    SocCheck last;
    const bool ok = readGaugePercentChecked(battery, percent, _socRejects, &last);
    // r163 E1: the DISPLAYED pipeline. The gate above is now only a sample source: the
    // number the UI gets is the 60-sample average, quantised down to 5 %, stepping at most
    // 5 % per 30 s in the direction the charge flag allows -- or 100 % while the E2
    // confirmation stands. A refused reading is fed as ok=false so the machine can see the
    // hole without inventing a sample.
    _socMachine.sample(now, ok, percent, _chargeFlag.raw(), _fullCharge.confirmed());
    // r98.3: the last gate verdict is kept so `batt` can print the raw word and the reason a
    // reading was refused, whether or not the command's own fresh read agrees.
    _socRaw = last.raw;
    memcpy(_socWhy, last.why, sizeof(_socWhy));
    if (!ok) {
      // r97: counted, not hidden. The UI still gets the last good value (a
      // percentage that flickers to 0 is worse than a stale one), but `batt`
      // can now say "no successful gauge read for N ms, K failures" instead of
      // implying the gauge is healthy.
      //
      // r98.1 D4: ... unless the last good value is older than BATTERY_STALE_MS. A gauge that
      // stopped answering mid-session used to freeze the UI at its last reading for the rest of
      // the session -- indistinguishable from a healthy pack, and wrong in the one direction that
      // matters (it hides a dying battery). Past the bound the UI is told 0 and `_socStale` makes
      // `batt` say why.
      ++_socFails;
      if (_socLastOkMs != 0 && (now - _socLastOkMs) > BATTERY_STALE_MS) {
        if (!_socStale) {
          _socStale = true;
          LOG_ERR("PWR", "gauge: no valid SoC for %lu ms (fails=%lu) -> UI reports 0, not the frozen %u%%",
                  static_cast<unsigned long>(now - _socLastOkMs), static_cast<unsigned long>(_socFails),
                  static_cast<unsigned>(_batteryCachedPercent));
        }
        return 0;
      }
      return _batteryCachedPercent;
    }
    // The displayed value is the MACHINE's, never the raw register: it is already
    // quantised down, filtered and rate-limited -- and pinned at 100 while the E2
    // confirmation stands, so "100 %" always has a 30 s voltage hold behind it.
    _batteryCachedPercent = _socMachine.hasValue() ? _socMachine.displayed() : percent;
    _socLastOkMs = now;
    _socStale = false;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  // LOCK1 (r101.9): this was a SINGLE SLOT. A second live Lock logged
  //   [ERR] [PWR] Lock already held, ignore
  // and held nothing (`valid = false`). The case that fired it on every sleep entry was the
  // render task's per-paint Lock arriving while enterDeepSleep()'s lock (main.cpp:2043) was
  // open for the whole sleep preparation: the sleep screen's paint is requested from inside
  // that scope, so the paint WAS at full speed on the opener's hold, but the nested take was
  // dropped -- the window would close with the FIRST releaser even with a nested user inside
  // it, and the error named nobody. Now every take counts, and the take says who opened.
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  const bool nested = powerManager.speedLockCount_ != 0;
  if (!nested) {
    powerManager.speedLockFirstOwner_ = self;
    powerManager.speedLockFirstOwnerName_[0] = '\0';
    strncpy(powerManager.speedLockFirstOwnerName_, pcTaskGetName(self),
            sizeof(powerManager.speedLockFirstOwnerName_) - 1);
    powerManager.speedLockFirstOwnerDepth_ = 0;
  }
  if (powerManager.speedLockFirstOwner_ == self) ++powerManager.speedLockFirstOwnerDepth_;
  ++powerManager.speedLockCount_;
  const uint16_t count = powerManager.speedLockCount_;
  char opener[sizeof(powerManager.speedLockFirstOwnerName_)];
  memcpy(opener, powerManager.speedLockFirstOwnerName_, sizeof(opener));
  opener[sizeof(opener) - 1] = '\0';
  valid = true;
  xSemaphoreGive(powerManager.modeMutex);
  if (nested) {
    // Not contention and not an error: the hold is counted, so both callers are protected for
    // their own full scope. AUD-P2-9: this fires about once per paint during sleep preparation, and
    // at INFO it pushed the [WAKE]/panel lines out of the 128 KB journal that is the only device
    // evidence there is. It is a debug line now -- the level was the defect, not the locking.
    LOG_DBG("PWR", "speed lock taken at depth=%u by %s (window opened by %s)",
            static_cast<unsigned>(count), pcTaskGetName(self), opener[0] != '\0' ? opener : "?");
  }
  // Immediately restore normal CPU frequency if currently in low-power mode
  powerManager.setPowerSaving(false);
}

HalPowerManager::Lock::~Lock() {
  if (!valid) return;
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.speedLockCount_ > 0) --powerManager.speedLockCount_;
  if (powerManager.speedLockCount_ == 0) {
    powerManager.speedLockFirstOwner_ = nullptr;
    powerManager.speedLockFirstOwnerName_[0] = '\0';
    powerManager.speedLockFirstOwnerDepth_ = 0;
  } else if (powerManager.speedLockFirstOwner_ == xTaskGetCurrentTaskHandle()) {
    // The opener's own hold ended but other tasks still hold: nothing here names them, so drop
    // the stale opener rather than credit the window to a task that has left.
    if (powerManager.speedLockFirstOwnerDepth_ > 0) --powerManager.speedLockFirstOwnerDepth_;
    if (powerManager.speedLockFirstOwnerDepth_ == 0) {
      powerManager.speedLockFirstOwner_ = nullptr;
      powerManager.speedLockFirstOwnerName_[0] = '\0';
    }
  }
  xSemaphoreGive(powerManager.modeMutex);
}
