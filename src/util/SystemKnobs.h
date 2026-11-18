#pragma once

#include <BoardConfig.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// Step 2.5 items 9 and 11 -- the two System-menu power knobs.
//
// This header is the ONE place the numbers live. The settings rows, the boot
// apply in main.cpp and the on-change apply in SettingsActivity all read the
// same tables, because a second copy of a VCOM value is a hardware risk and a
// second copy of a frequency is a setting that silently disagrees with itself.
//
// provenance (vendor clone v2.0.74, 2b2c36a):
//   item 9  main/display/screen/settings_screen/settings_power_tab.cc:26-31
//           kMhzOptions = {160, 240, 320}. The operator ADDED the 80 MHz step
//           below the vendor's floor (Roadmap, Step 2.5 scope). 80 MHz is the
//           frequency this build already idled at, so it is the default.
//   item 11 main/display/epd_refresh.cc:58
//           constexpr int kBlackDepthMv[] = {1200, 1600, 2000};
//           applied at :410 (boot) and :457 (on change, after epd_poweroff()),
//           with the labels Default / Medium / Strong (epd_refresh.h:8).
//           TAKEN EXACTLY. Nothing here may widen, round or re-derive them.
// ---------------------------------------------------------------------------
namespace systemKnobs {

// --- item 9: idle CPU frequency -------------------------------------------
inline constexpr uint8_t IDLE_FREQ_COUNT = 4;
inline constexpr uint16_t IDLE_FREQ_MHZ[IDLE_FREQ_COUNT] = {80, 160, 240, 320};

// --- item 11: panel VCOM / black depth ------------------------------------
inline constexpr uint8_t VCOM_COUNT = 3;
inline constexpr uint16_t VCOM_MV[VCOM_COUNT] = {1200, 1600, 2000};

// The VCOM rows exist only on the board whose PMIC is the TPS65185. Everywhere
// else the setting is absent from the list (and applyPanelVcom() is a no-op),
// so a shared binary cannot offer a knob the hardware does not have.
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
inline constexpr bool PANEL_VCOM_AVAILABLE = true;
#else
inline constexpr bool PANEL_VCOM_AVAILABLE = false;
#endif

// Index -> value. Out of range falls back to index 0 (80 MHz / 1200 mV), which
// is the shipped default, not an extrapolation.
uint16_t idleFreqMhz(uint8_t index);
uint16_t vcomMv(uint8_t index);

// Apply the persisted index. Both log once, every time they are called (boot and
// change), and both report what actually happened rather than what was asked for.
// applyPanelVcom() returns false when the value was refused by any of its gates --
// the caller must not claim success then. It STORES the value (r163 D9); the single
// VCOM register write per power-on is the driver's, in the between-scans window.
void applyIdleFreq(uint8_t index);
bool applyPanelVcom(uint8_t index);

}  // namespace systemKnobs
