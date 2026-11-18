#include "util/SystemKnobs.h"

#include <HalPowerManager.h>
#include <Logging.h>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <MetalioEInk4PlusBoard.h>
#include "metalio_vendor_panel.h"  // r163 D9: store-only VCOM path
#endif

namespace systemKnobs {

uint16_t idleFreqMhz(const uint8_t index) {
  return index < IDLE_FREQ_COUNT ? IDLE_FREQ_MHZ[index] : IDLE_FREQ_MHZ[0];
}

uint16_t vcomMv(const uint8_t index) { return index < VCOM_COUNT ? VCOM_MV[index] : VCOM_MV[0]; }

// --- item 9 ------------------------------------------------------------------
// The frequency itself is applied by HalPowerManager: it owns the idle/active
// transition, the mutex and the full-speed restore, and it is the only place that
// can change the clock without racing the idle policy. This function's job is the
// persisted index -> MHz mapping and the one log line.
void applyIdleFreq(const uint8_t index) {
  const uint16_t mhz = idleFreqMhz(index);
  if (index >= IDLE_FREQ_COUNT) {
    // A settings file from a newer build (or a corrupted byte): say so, then run
    // the default rather than an invented frequency.
    LOG_ERR("PWR", "idle cpu: step %u is not one of %u -> using %u MHz", static_cast<unsigned>(index),
            static_cast<unsigned>(IDLE_FREQ_COUNT), static_cast<unsigned>(mhz));
  }
  // Logs its own "idle cpu: set=<mhz> applied=<actual>" line, including at boot.
  powerManager.setIdleCpuFreqMhz(static_cast<int>(mhz));
}

// --- item 11 -----------------------------------------------------------------
bool applyPanelVcom(const uint8_t index) {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (index >= VCOM_COUNT) {
    LOG_ERR("PWR", "vcom: step %u is not one of %u -- refused, nothing changed", static_cast<unsigned>(index),
            static_cast<unsigned>(VCOM_COUNT));
    return false;
  }
  const uint16_t mv = VCOM_MV[index];

  // SECOND gate, against the board's own copy of the vendor ladder. The two must
  // agree; if they ever do not, the board's list wins and this refuses, because
  // the board's list is the one the power-on path actually writes.
  if (!freeink::metalio4plus::isPanelVcomLevel(mv)) {
    LOG_ERR("PWR", "vcom: %u mV is not a vendor black-depth level -- refused, nothing changed",
            static_cast<unsigned>(mv));
    return false;
  }

  // r163 D9: STORE, do not write. VCOM is the panel's bias, so a write from a settings
  // handler lands whenever the operator taps the row -- which is not provably between
  // scans, and a mid-frame bias change on this glass is a hardware risk rather than a
  // glitch. The driver keeps the value and writes it ONCE per power-on, from
  // epd_board_poweron() -> tps_set_vcom(vcom): rails ramping, output enable still LOW,
  // no waveform clocking, i.e. the between-scans window. Every drive goes through that
  // path (the PMIC is parked after each one), and this screen asks for a full refresh
  // after a change, so the new depth is what that drive uses.
  //
  // The previous version wrote VCOM1/VCOM2 over I2C from here and logged a read-back.
  // That is gone on purpose: the register write was the one thing in this path that
  // could race a scan, and the power-on write made it redundant anyway.
  if (metalio_vendor_panel_set_vcom_mv(static_cast<int>(mv)) != 0) {
    LOG_ERR("PWR", "vcom: %u mV refused by the driver -- stored value unchanged", static_cast<unsigned>(mv));
    return false;
  }
  // Report the value the driver holds, and say when it reaches the wire -- the honest
  // "applied" is the power-on write, not this call.
  LOG_INF("PWR", "vcom: set=%u stored=%d (no bus write here; the next panel power-on writes it between scans)",
          static_cast<unsigned>(mv), metalio_vendor_panel_vcom_mv());
  return true;
#else
  (void)index;
  LOG_ERR("PWR", "vcom: this board has no TPS65185 -- setting ignored");
  return false;
#endif
}

}  // namespace systemKnobs
