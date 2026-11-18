#pragma once

// ---------------------------------------------------------------------------
// PARK SINKS — Build A step 2 (E4c..E4f), docs/engineering/HARDWARE_POWER_PLAN.md §4/§6.
//
// WHY THIS EXISTS
//   The standby park (metalioStandbySleep in src/main.cpp) cuts the panel, the touch
//   controller, the EPD bus and two expander rails, and then light-sleeps in 60 s
//   slices. Four loads were still powered through every one of those slices:
//
//     usb    USB-Serial/JTAG PHY pads + pull-up        (this board's console lives there)
//     front  frontlight LEDC channels, duty 0 but still clocked and attached
//     radio  the esp_wifi stack (the RF was stopped, the driver and its buffers were not)
//     sd     the SDMMC pads, still claimed by the CNNT dedicated-pad controller
//
//   Each sink is cut at park and RESTORED on the matching resume path, and each is
//   individually switchable at runtime so ONE image can attribute the four in four
//   separate 1-hour coulomb windows (`exp park usb=0|1 front=0|1 radio=0|1 sd=0|1`).
//   A cut with no restore in the same code path would be a bug by definition.
//
// THE RULE (same as src/platform/ExpArms.h, and the reason this is safe to ship)
//   Every arm is a bool in RAM; the static initialiser is the DEFAULT and a reboot
//   is the reset. usb and radio default DISARMED (r106/r107); front and sd default
//   DISARMED since r118 (the verified pre-r105 park behaviour for both).
//   Nothing persists. `exp park` refuses while the device is parked, because an arm
//   changed half way through a park describes neither A nor B.
//
// BUILD-LEVEL KILL SWITCH
//   CP_PARK_SINKS=0 folds every predicate to `true` (armed) and compiles the hardware
//   legs out: the park path is then instruction-for-instruction the pre-batch one,
//   apart from four constant folds. None of these predicates is on the awake path.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

#ifndef CP_PARK_SINKS
#define CP_PARK_SINKS 1
#endif

namespace parksink {

enum class Sink : uint8_t {
  Usb = 0,    // E4c: USB-Serial/JTAG PHY pads down, gated on VBUS
  Front = 1,  // E4d: frontlight pads LOW, LEDC detached
  Radio = 2,  // E4e: esp_wifi_deinit (BT controller is not built into this image)
  Sd = 3,     // E4f: CNNT SDIO pads released (ded_sel=0, pull-downs on)
  Count = 4,
};

#if CP_PARK_SINKS

// True = this sink is cut at park. buildDefault() is the compiled default;
// active() is that value unless `exp park <name>=0` overrode it for this session.
//
// r106 B0: USB ships DISARMED. Measured on glass (R103_GLASS_LOG §F3): with the
// pads cut, a park left COM4 enumerated but unconfigurable
// (PermissionError(13, 'A device attached to the system is not functioning'),
// every command EMPTY after 0.2 s) -- a wedged CDC endpoint, i.e. it can cost the
// only instrument. `exp park usb=1` re-arms it for one bench session, RAM-only.
//
// r107 P6: RADIO ships DISARMED too, same shape and for the same reason (a sink that costs
// the instrument is not worth a measurement). esp_wifi_deinit() in park was a Build A
// experiment, not a proven saving: `exp park radio=1` still arms it for one session, and the
// park line says which switch armed it either way. SD and front stay ON -- they are the two
// the coulomb windows measured.
bool buildDefault(Sink s);
bool active(Sink s);
int8_t overrideValue(Sink s);  // -1 none, 0 forced off, 1 forced on
void setOverride(Sink s, bool on);
void clearAll();

const char* name(Sink s);  // "usb" | "front" | "radio" | "sd"
const char* help(Sink s);  // one sentence: what the sink cuts, and its gate

// One line per sink, greppable and always the same shape:
//   exp park usb=on default=on (USB-Serial/JTAG PHY pads down in park when VBUS is absent)
size_t status(char* out, size_t cap);

// r109 EXP2: the same four lines WITHOUT the parenthetical help, for `exp park` with no args --
// four short `exp park <name>=<on|off> default=<on|off>` lines, small enough for the shell's body
// cap where the full form is not.
size_t statusCompact(char* out, size_t cap);

// ---- hardware legs --------------------------------------------------------
// Each returns true ONLY when the state read back as written; a false return means
// the sink did not move, and the caller logs that instead of crediting it.
bool usbPhyDown();  // usb_pad_enable=0 (PHY pads + D+ pull-up), driver and clock untouched
bool usbPhyUp();    // usb_pad_enable=1 -> the host re-enumerates on the next attach
bool sdPadsRelease();
bool sdPadsRestore();

// E4e. `wasInited` is false when esp_wifi was never initialised in this session
// (the common case on battery) -- then nothing was cut and nothing is restored.
// `err` carries the esp_err_t from whichever call ran.
bool radioDown(bool* wasInited, int* err);
bool radioUp(int* err);  // only meaningful after radioDown() reported wasInited

// ---- r106 read-out ------------------------------------------------------------------
// The `sd` and `radio` shell commands must report what the LAST park actually did,
// not what the arms say it would do. These are set by the hardware legs below and
// never by a command, so they are evidence rather than intent.
enum class Verdict : int8_t { Never = -1, Ok = 0, Failed = 1 };

Verdict lastSdRelease();  // result of the last sdPadsRelease()
Verdict lastSdRestore();  // result of the last sdPadsRestore()
bool sdPadsReleased();    // a release landed and no restore has run since
Verdict lastRadioCut();   // result of the last radioDown()
bool radioDeinited();     // esp_wifi_deinit() was applied and has not been re-inited

#else  // CP_PARK_SINKS=0 — the pre-batch park path, folded

constexpr bool buildDefault(Sink) { return true; }
constexpr bool active(Sink) { return true; }
constexpr int8_t overrideValue(Sink) { return -1; }
inline void setOverride(Sink, bool) {}
inline void clearAll() {}
inline const char* name(Sink) { return "off"; }
inline const char* help(Sink) { return ""; }
inline size_t status(char* out, size_t cap) {
  if (out != nullptr && cap != 0) out[0] = '\0';
  return 0;
}
inline size_t statusCompact(char* out, size_t cap) {
  if (out != nullptr && cap != 0) out[0] = '\0';
  return 0;
}
constexpr bool usbPhyDown() { return false; }
constexpr bool usbPhyUp() { return false; }
constexpr bool sdPadsRelease() { return false; }
constexpr bool sdPadsRestore() { return false; }
constexpr bool radioDown(bool*, int*) { return false; }
constexpr bool radioUp(int*) { return false; }
constexpr Verdict lastSdRelease() { return Verdict::Never; }
constexpr Verdict lastSdRestore() { return Verdict::Never; }
constexpr bool sdPadsReleased() { return false; }
constexpr Verdict lastRadioCut() { return Verdict::Never; }
constexpr bool radioDeinited() { return false; }

#endif

}  // namespace parksink
