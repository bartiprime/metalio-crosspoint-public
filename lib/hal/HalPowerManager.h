#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>
#include <freertos/task.h>  // LOCK1: TaskHandle_t + pcTaskGetName() for the lock owner in the log

#include <cassert>

#include "HalGPIO.h"
#include "util/BatteryGaugeState.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
 public:
  // The last frequency transition setPowerSaving() actually made, and what it
  // does NOT say: nothing here knows WHY the caller asked, because
  // setPowerSaving() has no reason argument. The only caller that moves this on
  // a real device is main.cpp's idle policy (320 -> 80 after 3 s idle).
  struct Transition {
    uint32_t atMs = 0;  // millis() of the change; 0 = none since boot
    bool toLowPower = false;
  };

 private:
  int normalFreq = 0;  // MHz
  bool isLowPower = false;
  // Step 2.5 item 9: the frequency setPowerSaving() enters at idle. Starts at the
  // shipped LOW_POWER_FREQ and is moved by the persisted setting; read/written
  // only under modeMutex in setIdleCpuFreqMhz(). See the setter's comment.
  int idleFreqMhz_ = LOW_POWER_FREQ;
  // r99.5 DRIVE5: how many callers currently HOLD full speed (see FullSpeedHold). Read and
  // written only under modeMutex, i.e. in the same critical section that applies the switch,
  // so "is anything holding full speed?" and "apply the frequency" cannot be separated.
  int fullSpeedHolds_ = 0;
  // r96: last actual frequency transition, for the `pm` readout. Written only
  // under modeMutex, read without it (a torn read at worst reports one change late).
  mutable Transition lastTransition_;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
  mutable bool _chargingCached = false;          // Last read charge state
  mutable unsigned long _chargingLastPollMs = 0; // Timestamp of last charge-state read
  // r97 BATT1 diagnostics. The gauge can fail transiently and every failure here
  // is SILENT: getBatteryPercentage() returns the previous value and
  // isCharging() caches `false` for the whole 1.5 s window, so a dead read and a
  // genuinely-not-charging battery look identical to the UI. These three fields
  // make the difference visible to the `batt` shell command without changing what
  // the UI is told.
  mutable unsigned long _socLastOkMs = 0;   // millis() of the last SUCCESSFUL gauge read; 0 = none
  mutable unsigned long _socLastPollMs = 0; // millis() of the last ATTEMPT
  mutable uint32_t _socFails = 0;           // readPercentageChecked() failures since boot
  // r98.1 D2/D4 diagnostics: why a read was refused, and whether the value the UI is
  // showing has outlived its usefulness.
  mutable uint32_t _socRejects = 0;         // reads that answered but failed the D2 validity gate
  mutable bool _socStale = false;           // last-good value is older than BATTERY_STALE_MS
  // r98.3: the raw 0x2C|0x2D word and the exact reason the last poll's reading was refused,
  // kept so `batt` can show both without repeating the gate's logic (and possibly disagreeing).
  mutable uint16_t _socRaw = 0;
  mutable char _socWhy[128] = {0};

  // r163 E3: the charge truth table, with its +-5 mA deadband (src/util/BatteryGaugeState.h).
  // Written only by the poll inside isCharging(); read without a lock, like every other
  // cache here.
  mutable gaugestate::ChargeFlag _chargeFlag;

  // r163 E2: the full-charge confirmation (4300 mV held 30 s, exit at 4280 mV). RAM only:
  // a persisted "confirmed at 100 %" would outlive the battery it describes. See
  // src/util/BatteryGaugeState.h's FullCharge note and notifyStandbyWake().
  mutable gaugestate::FullCharge _fullCharge;
  mutable uint16_t _gaugeMv = 0;      // last 0x08 sample fed to the confirmation
  mutable bool _gaugeMvOk = false;    // ... and whether that read succeeded

  // r163 E1: the displayed-SoC pipeline (60-sample average -> quantise down to 5 % ->
  // 5 %/30 s -> one-shot post-wake recalibration). RAM only, like the confirmation above:
  // a displayed percentage restored from NVS would be a number no sample supports.
  mutable gaugestate::SocMachine _socMachine;

  // r163 E3: one windowed poll of every charge-state source (gauge Current(), then the
  // charger IC, then the SDK), shared by isCharging()/isChargingDisplayed()/pollBattery().
  // Defined further down the .cpp, next to the guarded I2C helpers it uses.
  void refreshChargeState(unsigned long now) const;

  // r163 E2: one 0x08 sample into the full-charge confirmation. Defined next to the
  // guarded I2C helpers; a no-op on boards without the Metalio gauge.
  void feedFullCharge(unsigned long now) const;

  // ---------------------------------------------------------------------------
  // LOCK1 (r101.9): the Lock below is a COUNTED WINDOW, not a single slot.
  //
  // It used to be `enum LockMode { None, NormalSpeed }` -- one slot. The second live Lock
  // logged `[ERR] [PWR] Lock already held, ignore` and held NOTHING, so the only thing keeping
  // the chip fast was the FIRST holder's scope. That is safe only while the opener outlives
  // every nested user, and the case the operator sees on every sleep entry is exactly that
  // shape: enterDeepSleep() (main.cpp) takes the lock for the whole sleep preparation, the
  // sleep screen's paint is requested from INSIDE that scope (`enter paint owed`, requested at
  // the transition) and is composed on the render task, whose own Lock was refused. So the
  // paint was covered -- by main's hold -- while its own Lock was invalid, and the ERR named
  // nobody.
  //
  // Counting the holders makes the nested take REAL rather than ignored: the window stays open
  // until the LAST releaser, so a nested user can never outlive the protection it asked for,
  // and the take names the task that opened the window. Neither the frequency policy nor its
  // transitions change: setPowerSaving() still refuses low power exactly while the count is
  // non-zero, which is the old `currentLockMode != None` test.
  uint16_t speedLockCount_ = 0;                 // live Lock instances; low power only at 0
  TaskHandle_t speedLockFirstOwner_ = nullptr;  // task that took the oldest live hold
  char speedLockFirstOwnerName_[24] = {0};      // its name, captured at take time (no dangling handle)
  uint16_t speedLockFirstOwnerDepth_ = 0;       // how many live holds belong to that task
  SemaphoreHandle_t modeMutex = nullptr;  // Protects speedLockCount_ and the owner fields

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  // -------------------------------------------------------------------------
  // Step 2.5 item 9 — the IDLE FREQUENCY IS A SETTING, not a constant.
  //
  // LOW_POWER_FREQ stays as the shipped floor and the default: it is what every
  // build ran before this setting existed, and the value the `pm` readout and the
  // non-PSRAM boards still mean. The vendor exposes 160/240/320 MHz as an idle
  // choice (settings_power_tab.cc:26-31, kMhzOptions) and the operator added an
  // 80 MHz step below their floor, so this board offers {80,160,240,320}.
  //
  // 80 MHz is the risky step and the reason this is a setting rather than a
  // policy change: it is BELOW the vendor's floor, and the panel path on this
  // board is timing-sensitive. Nothing in this file's restore path changes --
  // the full-speed restore is still setCpuFrequencyMhz(normalFreq) on the same
  // edges -- so a flaky 80 MHz is a setting the operator can leave at 160..320.
  // -------------------------------------------------------------------------
  void setIdleCpuFreqMhz(int mhz);
  int idleCpuFreqMhz() const { return idleFreqMhz_; }

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // ---------------------------------------------------------------------------
  // r99.5 DRIVE5 -- HELD RESOURCE, NOT A PREDICATE.
  //
  // Report r99.4: the unit panicked 1 ms after the USB MSC gadget started when drive mode was
  // armed by the CABLE appearing on the refuse screen. The ordering that allowed it was:
  // main loop read `requiresExclusiveStorageLoop()`/`preventAutoSleep()` (both `sessionStarted`)
  // while still false, the activity's loop() then armed in the SAME iteration, and the loop's
  // already-decided `setPowerSaving(true)` was applied after the gadget was live. A predicate
  // that is read before the event it describes cannot protect the event.
  //
  // So the USB window takes a hold instead. It is acquired BEFORE anything is detached or
  // routed, spans the whole armed session (the arm transition, the gadget start, the host
  // transfer and the teardown), and is released only when the session is over. While any hold
  // exists setPowerSaving() refuses to enter low power -- checked under the SAME mutex that
  // applies the switch, so no task can slip a frequency change into the window.
  //
  // Reference-counted on purpose: a hold has to span a whole armed session while the render
  // task keeps taking its own per-paint Lock, and it has to be readable as a predicate (`pm`).
  class FullSpeedHold {
   public:
    FullSpeedHold() = default;
    ~FullSpeedHold() { release(); }
    FullSpeedHold(const FullSpeedHold&) = delete;
    FullSpeedHold& operator=(const FullSpeedHold&) = delete;

    // Takes the hold (refcounted) and returns immediately at normal frequency: the caller's
    // next step may already need the PLL at full speed, so the restore happens HERE.
    void acquire(const char* reason);
    void release();
    bool held() const { return held_; }

   private:
    bool held_ = false;
  };

  // Diagnostics for the `pm` readout: is a hold in force, and how many.
  bool fullSpeedHeld() const;
  int fullSpeedHoldCount() const;

  // -------------------------------------------------------------------------
  // r96 READ-ONLY ACCESSORS for the `pm` shell command (src/platform/CpuStats.cpp).
  // They change no behaviour: they report the state setPowerSaving() already
  // holds. Added here rather than reading the fields from outside because
  // speedLockCount_ is mutex-protected and normalFreq is private.
  // -------------------------------------------------------------------------
  bool lowPowerActive() const { return isLowPower; }
  int normalFreqMhz() const { return normalFreq; }
  bool normalSpeedLockHeld() const;

  // The last frequency transition (see the Transition struct at the top of the
  // class; defined there so it can also be a member's type).
  Transition lastTransition() const;

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the speed lock
  // wakeOnHeldButton: arm the wake without waiting for the power button's
  // release, so a press that is still down wakes the device straight back up
  // through the normal power-button wake path.
  void startDeepSleep(HalGPIO& gpio, bool wakeOnHeldButton = false) const;

#if FREEINK_DEVICE_PICCO
  // While USB powers the device, light-sleep instead of deep sleep (deep sleep
  // cannot observe the charger), calling onStatus whenever the charging state
  // or percentage changes. Returns true when the power button ended the hold,
  // false when USB went away or was never present.
  bool holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const;
#endif

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // BATT6: is the DISPLAYED percentage still mid-recovery from a standby resume?
  //
  // notifyStandbyWake() opens a 3 s settle and arms the one-shot recalibration, and
  // during that window getBatteryPercentage() deliberately returns the HELD
  // pre-sleep value (BatteryGaugeState.h: "samples during it are not evidence").
  // Anything that records a SAMPLE -- not a display -- has to wait for the first
  // post-wake sample to land, or it books the value the pack had before the sleep
  // as the current one. BatteryStats::update() is exactly such a recorder.
  //
  // False on boards with no gauge (nothing to wait for) because the machine is
  // never driven there: it starts settled with no recalibration armed.
  bool socAwaitingPostWakeSample() const {
    return _socMachine.settling() || _socMachine.recalibrationPending();
  }

  // r98.1 D4 — how long a last-good SoC may stand in for a failing gauge before
  // the UI is told 0 and `batt` reports it as stale. 30 s is 20 missed polls at
  // the 1500 ms cache window: long enough that a single failed read does not
  // blank the icon, short enough that a wedged gauge cannot hide a dying pack
  // for a whole session.
  static constexpr uint32_t BATTERY_STALE_MS = 30000;

  // -------------------------------------------------------------------------
  // r97.1 BATT3 — the UI reads a CACHE, so the cache has to be refreshed by
  // something other than the UI. getBatteryPercentage() / isCharging() only
  // re-read the hardware when their own BATTERY_POLL_MS (1500 ms) window has
  // expired, and nothing called them while the status line was not being
  // repainted -- measured on the device as `ui 96% age=26717 ms`. This is the
  // missing timer: the main loop calls it once per iteration, and the 1500 ms
  // windows below do the gating, so it costs one gauge read and one charger read
  // per 1.5 s of AWAKE time regardless of how rarely the UI asks. It does no I2C
  // while the device is in standby (loop() does not run there) and it is not a
  // busy poll (two millis() compares per loop pass, which is already delaying
  // 10-50 ms).
  // -------------------------------------------------------------------------
  void pollBattery() const;

  // -------------------------------------------------------------------------
  // r97.1 BATT3 — what the charge flag ACTUALLY reads. The board's charger is a
  // CX25601N at 0x6B (`MetalioEInk4PlusBoard.h:59`), not the BQ25896 the shared
  // SDK driver assumes: same address, different register map, so
  // BatteryMonitor's REG0x0B CHRG_STAT[4:3] read lands on the CX25601N's
  // IOTG_HI byte and can never report charging. Charge status on this part is
  // REG0x1E, CHG_STAT[4:3] (vendor: main/boards/common/power/cx25601n.cc:38
  // REG_STATUS1, cx25601n_get_chrg_stat() read_bits(REG_STATUS1, 0x03, 3);
  // 0 = terminated/not charging, 1 = CC, 2 = CV, 3 = top-off).
  // Filled in fresh for the `batt` command; no caching here.
  // -------------------------------------------------------------------------
  struct ChargerDiag {
    uint8_t chargerAddr = 0;   // BoardConfig charger address (0 = none)
    bool cxIdentified = false; // the 0x6B part answered as a CX25601N
    bool statusRead = false;   // REG0x1E read succeeded this call
    uint8_t reg1E = 0;         // REG0x1E raw
    int chgStat = -1;          // CHG_STAT[4:3], -1 = unknown
    bool charging = false;     // flag this register derives
    bool partRead = false;     // PART_INFO (0x38) read succeeded
    uint8_t partInfo = 0;      // PART_INFO raw
  };
  ChargerDiag chargerDiag() const;

  // -------------------------------------------------------------------------
  // r97 READ-ONLY ACCESSOR for the `batt` shell command (src/debug/UsbShell.cpp).
  // Reports what the cached readouts actually did -- it changes no behaviour and
  // performs no I/O: the command does its own fresh gauge reads and uses this
  // only to age the value the UI is currently showing.
  //
  // HONEST LIMIT, stated here because it decides what `batt` can claim:
  // BatteryMonitor::isCharging() returns a bare bool, so a FAILED charger/gauge
  // read is indistinguishable from "not charging" THROUGH THIS API. That is
  // exactly the ambiguity the command exists to expose, so `batt` reads the two
  // I2C registers itself (0x0B charger status, 0x0A gauge status) and reports
  // its own success/failure separately.
  // -------------------------------------------------------------------------
  struct BatteryDiag {
    unsigned long socLastOkMs = 0;    // millis() of the last successful gauge read (0 = never)
    unsigned long socLastPollMs = 0;  // millis() of the last attempt
    uint32_t socFails = 0;            // failures since boot
    uint32_t socRejects = 0;          // answered but failed the validity gate (D2)
    bool socStale = false;            // last good value is older than BATTERY_STALE_MS (D4)
    bool socEverRead = false;         // at least one successful read since boot
    uint16_t socCached = 0;           // the last good percentage (what the UI shows while fresh)
    uint16_t socRaw = 0;              // raw 0x2C|0x2D word from the LAST gate attempt (r98.3)
    char socWhy[128] = {0};           // why that attempt was refused; empty when it passed
    unsigned long chargingLastPollMs = 0;  // millis() of the last isCharging() refresh
    bool chargingCached = false;           // the value the UI would be shown
    // r163 E2: the full-charge confirmation, so `batt` can show a latched 100 % and how
    // long the current 4300 mV hold has stood. RAM only; never persisted.
    bool fullConfirmed = false;
    bool fullHolding = false;
    uint32_t fullHeldMs = 0;
    uint16_t gaugeMv = 0;     // last 0x08 sample the confirmation was fed
    bool gaugeMvOk = false;   // ... and whether that read succeeded
    // r163 E1: the displayed-SoC pipeline's own state, so `batt` can separate "the gauge
    // says N %" from "the machine shows M %".
    uint8_t socSamples = 0;        // samples in the 60-sample window
    uint16_t socAverage = 0;       // their integer mean (pre-quantisation)
    bool socSettling = false;      // inside the post-wake 3 s settle
    bool socRecalPending = false;  // the one-shot recalibration is still armed
    unsigned long uptimeMs = 0;            // millis() when this snapshot was taken
  };
  BatteryDiag batteryDiag() const;

  // -------------------------------------------------------------------------
  // r98.3 -- THE SoC PLAUSIBILITY GATE, AS ONE FUNCTION, SHARED BY EVERY CALLER.
  //
  // Root cause of the observed `soc 65380%` (0xFF64): 0x2C StateOfCharge is ONE byte, but the
  // SDK path and this file's own `batt` printer both read it as a little-endian u16
  // (BatteryMonitor.cpp:367 readReg16(0x2C) -> `out = soc > 100 ? 100 : soc`). The second byte
  // comes from 0x2D, which is not part of the register and answers 0xFF, so a perfectly good
  // 100% read (low byte 0x64) became 0xFF64 = 65380 and the SDK's clamp promoted it to 100%.
  //
  // So: read 0x2C as ONE byte, twice, independently. Accept only if the two reads agree, both
  // are <= 100, and the old 16-bit read's high byte (0x2D) would have been zero, i.e. the value
  // is a plain percentage. Every failure is refused with a printable reason -- never clamped,
  // because clamping turns the one unambiguous failure (a garbage read) into a full battery.
  // -------------------------------------------------------------------------
  struct SocCheck {
    bool ok = false;
    bool answered = false;  // at least one 0x2C byte read succeeded (vs an I2C NAK)
    uint8_t soc1 = 0;       // first 1-byte read of 0x2C
    uint8_t soc2 = 0;       // the independent re-read
    uint8_t spare = 0;      // 0x2D, the byte a 16-bit read used to pick up as the high byte
    uint16_t raw = 0;       // (spare << 8) | soc1 -- exactly what the old 2-byte read returned
    uint16_t soc = 0;       // valid only while ok
    char why[128] = {0};    // empty when ok, otherwise the failing test(s), printable
  };
  static SocCheck readSocChecked(uint8_t addr);

  // r98.3 -- validated RemainingCapacity (0x10) read, shared by `batt` and the standby
  // wake-time recorder so the two can never disagree about what the coulomb counter said.
  // 0x10 IS a 16-bit register (unlike 0x2C): two independent reads, which must agree, and a
  // sanity bound -- a coulomb count above 20000 mAh cannot describe this pack.
  struct CapCheck {
    bool ok = false;
    bool answered = false;  // at least one 0x10 read succeeded
    uint16_t mah = 0;       // valid only while ok
    uint16_t mah2 = 0;      // the independent re-read, kept for the reason string
    char why[96] = {0};
  };
  static CapCheck readRemainingCapacityChecked(uint8_t addr);

  // True when the battery is ACTIVELY charging, not merely when a cable is
  // attached: BatteryMonitor picks the board's real charge signal (gauge
  // Current() sign, /STAT pin, or a charger IC's CHRG_STAT). Cached over the
  // same window as the percentage because the status line calls it per frame.
  bool isCharging() const;

  // -------------------------------------------------------------------------
  // r163 E3 -- THE VALUE A USER IS SHOWN, which is NOT isCharging().
  //
  // isCharging() is the RAW verdict: current > +5 mA, with a +-5 mA deadband so a pack
  // at rest cannot flip it (src/util/BatteryGaugeState.h). That is the right input for a
  // DECISION -- BatteryStats' charge sessions must keep counting through the CV tail,
  // because the pack really is still taking charge there.
  //
  // This is the DISPLAYED verdict: raw charging AND displayed SoC < 100, so the lightning
  // bolt is not drawn while the pack sits full on the cable. Use it wherever a person can
  // see the answer -- the status-bar glyph (BaseTheme/LyraTheme), the icon-repaint claim
  // (BatteryIconRegion) and its edge detector.
  //
  // Neither is "a cable is attached": isExternalPowerPresent() is that question, and it
  // is a third one (a full pack on the cable is powered and not charging).
  // -------------------------------------------------------------------------
  bool isChargingDisplayed() const;

  // -------------------------------------------------------------------------
  // r163 E2 -- THE STANDBY RESUME HOOK for the gauge state machines.
  //
  // One call per standby resume, after the rails and the I2C bus are back and BEFORE the
  // first post-wake gauge sample (main.cpp calls it next to wakerecResume()). It:
  //   * keeps the full-charge confirmation (the pack did not stop being full because the
  //     CPU slept) and re-validates it against the first post-settle voltage;
  //   * clears the SoC filter and opens the 3 s settle + one-shot recalibration;
  //   * restarts the "last good SoC" clock, so the >30 s stale rule does not blank the
  //     icon during the settle (the displayed value is held, not zeroed).
  // Nothing is read here and nothing is persisted: the settle is a deadline, and the first
  // sample after it is the evidence.
  // -------------------------------------------------------------------------
  void notifyStandbyWake();

  // -------------------------------------------------------------------------
  // r97 BATT2 -- THE ONE FUNCTION the battery-statistics charge-session logic
  // reads the charge state through.
  //
  // r163 E3: this stays the RAW flag on purpose. The CV tail is still a charge session --
  // the pack is taking current and the session must not end a minute early -- while the
  // bolt is hidden at 100 %. isChargingDisplayed() is the UI's answer; this is the
  // record's.
  //
  // The flag underneath used to be suspect (the operator measured ~1 A into the pack
  // while the UI showed not-charging), and BATT1's `batt` command exists to pin down why.
  // Keeping the stats behind this function means a corrected rule -- gauge Current()
  // sign WITH a deadband (r163 E3, src/util/BatteryGaugeState.h), charger STAT, USB
  // presence AND current, whatever the evidence supports -- is a change HERE and nowhere
  // else: the stats record, its reset edges and the page all follow it automatically.
  // -------------------------------------------------------------------------
  bool chargingForStats() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
