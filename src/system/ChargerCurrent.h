#pragma once

#include <stdint.h>

// CX25601N charge-current limit -- STEP 2.5 item 8. Deliberately NOT a driver port.
//
// THE DEFECT THIS EXISTS FOR: our firmware never wrote a charger register at all
// (main.cpp zeroes `chargerAddr` on this board on purpose, so the SDK's BQ25896 path
// is unreachable), which means the CX25601N runs on whatever its power-on state is
// and nobody could name the charge rate. UsbShell's `batt` note records the
// measurement that started this: ~1 A from the wall at 87 %.
//
// The vendor's `cx25601n.cc` is 697 lines, but almost all of it is telemetry, OTG and
// interrupt handling. The LIMIT is two registers and the arithmetic below, and these
// values are the vendor's own (upstream 2b2c36a):
//   main/boards/common/power/cx25601n.h   -- address, MIN/MAX/STEP
//   main/boards/common/power/cx25601n.cc  -- the bit layout and the two writes
//   settings_charging_tab.cc:7            -- the three UI values
//   metalio_power.cc:20-22                -- their board default (1 A) and allow-list
//
//   REG_ICHG_LO 0x02 bits 7:6 = ICHG code bits 1:0
//   REG_ICHG_HI 0x03 bits 3:0 = ICHG code bits 5:2
//   ICHG = code * 80 mA, code clamped to 1..38 i.e. 80..3040 mA
//   I2C 0x6B (7-bit), same bus as the BQ27220 gauge.
//
// Both writes are read-modify-write. That matters: the vendor's own `update_bits()`
// does the same, because the neighbouring bits in 0x02/0x03 belong to other features
// and a blind byte write would clear them.
namespace chargercurrent {

constexpr uint8_t kAddr = 0x6B;  // CX25601N_I2C_ADDR (7-bit)
constexpr uint8_t kRegIchgLo = 0x02;
constexpr uint8_t kRegIchgHi = 0x03;

constexpr uint16_t kMinMa = 80;    // CX25601N_ICHG_MIN_MA
constexpr uint16_t kMaxMa = 3040;  // CX25601N_ICHG_MAX_MA -- never exceed this
constexpr uint16_t kStepMa = 80;   // CX25601N_ICHG_STEP_MA
constexpr uint8_t kMaxCode = 38;   // the vendor's own code clamp (3040 / 80)

// The three values the System row offers, verbatim from the vendor's UI
// (settings_charging_tab.cc:7). 1.5 A is the top of the RANGE WE SURFACE, not of the
// part; do not add a fourth value without a measurement that separates it.
constexpr uint8_t kChoiceCount = 3;
constexpr uint16_t kChoicesMa[kChoiceCount] = {500, 1000, 1500};
// 1 A: the vendor board's own default (metalio_power.cc:20 `s_current_ma{1000}`).
constexpr uint8_t kDefaultChoice = 1;

// Index -> mA, clamped to the table so a corrupted settings file cannot make this
// write an out-of-range current.
uint16_t maForChoice(uint8_t choice);

// What the part actually holds, decoded from REG0x02/0x03. `code` is the raw 6-bit
// field; `appliedMa` (optional) is code * 80. False when either register did not
// answer -- an absent charger (no cable is irrelevant; the IC is battery-powered, but
// a NACK means the bus/part is not answering) must not look like a valid 0 mA.
bool readCode(uint8_t& code, uint16_t* appliedMa = nullptr);

// Clamp -> quantise -> two read-modify-writes -> read back.
//
// Fails harmlessly and logs when the part does not answer: each write's own read-modify-write read
// is the presence test, so a dead bus can never leave a half-written register pair. Returns true
// only when both writes succeeded AND the read-back code matches what was written.
//
// There is deliberately NO read of the old value before the writes: `was=` was removed because it
// only reported the power-on default the IC had been living with. An IC that does not answer at
// 0x6B still produces an unconditional ERR ("set=<n>mA NOT applied -- no answer at 0x6B"), which is
// the one case that must be visible with the runtime debug switch off.
bool applyMa(uint16_t ma, uint16_t* appliedMaOut = nullptr, uint8_t* codeOut = nullptr);

// Settings-row entry point: choice index -> applyMa, with the one [PWR] line.
bool applyChoice(uint8_t choice, uint16_t* appliedMaOut = nullptr);

// ---------------------------------------------------------------------------
// r117 BUCKET A — the vendor's charge-termination default (cx25601n.cc:322-323).
//
// The vendor's cx25601n_init() writes THREE things we never wrote: VREG=4400 mV,
// ICHG=500 mA and EN_TERM. ICHG we already own (applyMa). VREG is the constant-
// voltage target the pack is charged to, and EN_TERM is the CHG_CTRL0 bit that
// lets the part end the charge itself -- without it the termination test is off
// and the charger relies on its own power-on default.
//
//   REG0x04 bits 7:3 = VREG code bits 4:0   REG0x05 bits 3:0 = VREG code bits 8:5
//   code = mV / 10   (CX25601N_VREG_STEP_MV), range 3840..4800 mV
//   REG0x14 bit 2    = EN_TERM (cx25601n.cc:132, via update_bits)
//
// PURELY the charger: no panel, no rail, no expander, no pad mux, no park/resume
// step. It cannot influence banding, which is why it ships in the same image as
// the r117 park reorder. Each write is a read-modify-write, so the neighbouring
// field (0x04 carries ITERM/charge-enable neighbours on this part) cannot be
// cleared by it. Returns true only when both registers were written AND VREG
// reads back as written; a part that does not answer logs and changes nothing.
// ---------------------------------------------------------------------------
constexpr uint16_t kVregMv = 4400;  // the vendor's `default vreg` (cx25601n.cc:322)
bool applyVendorVreg(uint16_t* appliedMvOut = nullptr);
}  // namespace chargercurrent

// ---------------------------------------------------------------------------
// r163 E4 -- THE RUNTIME CHARGER CONTROL LOOP (see src/system/ChargerPolicy.h).
//
// THE DEFECT THIS EXISTS FOR. Until now this firmware wrote the charger exactly ONCE per
// boot (`applyChoice` + `applyVendorVreg`) and then never touched it again: no EN_TERM
// gating, no recharge kick, no undervoltage protection, and no record of what it wrote. The
// part was left to its own devices, which on this pack means a charge that terminates at the
// ITERM comparator before the cut-off and then never restarts.
//
// KILL SWITCH -- `CP_CHARGER_CONTROL`, default 1 (the enabled path).
//   WITH IT ON : the tick below runs on the main loop, at 2 s below the cut-off and 10 s at
//                or above it, and writes ONLY what the policy names: EN_TERM (0x14 bit 2) and
//                EN_CHG/HIZ (0x16 bits 5/4) for a recharge kick. VREG is written once per boot.
//                UVLO is DETECTED and journaled but never acted on: REG0x18 (BATFET shutdown) is
//                one-way in software, so this build does not write it -- see ChargerPolicy.h and
//                docs/engineering/R163_ITEM_E4.md §6.
//   WITH IT OFF: `tick()` is compiled out and returns false before touching Wire, so the
//                charger behaves exactly as it did before this item: one boot-time write of
//                VREG=4400 mV and EN_TERM=1, and nothing else, ever. Nothing else in the
//                image changes -- no other file reads this macro.
// Every write in the ON path is journaled (register, old byte, new byte, the bits changed,
// and the reason) as one `[PWR] charger-write` line, so a glass session can be judged from
// the log alone.
#ifndef CP_CHARGER_CONTROL
#define CP_CHARGER_CONTROL 1
#endif

namespace chargercontrol {

// One poll, if one is due. Reads the pack and the charger, asks the policy for one plan, and
// performs at most that plan's writes. Returns true when a write was actually made, so the
// caller can count them. Never throws, never blocks longer than the I2C timeouts (50 ms).
bool tick(uint32_t nowMs);

// Why the last tick did what it did (a static string; "" before the first tick). The one
// line the boot/battery diagnostics print, and the reason every journal line carries.
const char* lastReason();

// Total writes made by this loop since boot, for the `batt` readout. A write is counted
// ONLY when the register byte actually changed -- a plan whose target value is already in
// the register performs no I/O at all and is not a write.
uint32_t writeCount();

}  // namespace chargercontrol

