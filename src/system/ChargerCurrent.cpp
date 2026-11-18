#include "system/ChargerCurrent.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <Wire.h>

#include <algorithm>
#include <cstdio>

#include "system/ChargerPolicy.h"

namespace chargercurrent {
namespace {

constexpr int kTimeoutMs = 50;  // short: this runs on the boot path and on the UI thread

// Same wire shape as UsbShell's battRead8/battRead2: write the register, repeated
// start, read one byte. Duplicated here rather than shared because those are
// file-static in the shell, and the shell is a diagnostic surface -- a settings path
// must not depend on it.
// BUS INVARIANT: ONE shared I2C bus, the same wire the TCA9555 expander lives on;
// the expander's failed read is what caused the power-key glitch. This is one of
// four accessor sets on it (here, HalPowerManager, UsbShell, and
// MetalioEInk4PlusBoard.h, where the canonical note lives) and it takes no bus
// guard, so this transaction can interleave with expander/EPD traffic. Never
// "fix" a failed read by freeing the bus, and do not fold these sets together
// without a measurement round.
bool read8(const uint8_t reg, uint8_t& out) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kAddr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1) return false;
  out = static_cast<uint8_t>(Wire.read());
  return true;
}

bool write8(const uint8_t reg, const uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

// The vendor's update_bits(), same mask/shift semantics: read the byte, replace only
// the field, write it back. A failed read aborts, never a blind write.
//
// This read is ALSO the charger's presence test: it is the first access applyMa() makes, so an IC
// that does not answer at 0x6B is caught before any register is modified. The presence test used to
// sit in a separate read in front of the writes, because the value it returned was reported as
// `was=`; that field is gone (it only restated the power-on default the operator had been living
// with), so the read that produced it is gone too -- the VERDICT is not. Absence must stay visible
// without the runtime debug switch, hence ERR and the same "NOT applied ... no answer at 0x6B"
// wording the old probe used.
bool updateBits(const uint8_t reg, const uint8_t mask, const uint8_t shift, const uint8_t field, const char* what,
                const uint16_t setMa) {
  uint8_t cur = 0;
  if (!read8(reg, cur)) {
    LOG_ERR("PWR", "charger current: set=%umA NOT applied -- no answer at 0x%02X (REG0x%02X, %s); "
                   "the IC keeps its power-on value",
            static_cast<unsigned>(setMa), kAddr, reg, what);
    return false;
  }
  const uint8_t m = static_cast<uint8_t>(mask << shift);
  const uint8_t next = static_cast<uint8_t>((cur & static_cast<uint8_t>(~m)) | ((field << shift) & m));
  if (!write8(reg, next)) {
    LOG_ERR("PWR", "charger current: REG0x%02X write failed (%s)", reg, what);
    return false;
  }
  return true;
}

}  // namespace

uint16_t maForChoice(const uint8_t choice) {
  return kChoicesMa[choice < kChoiceCount ? choice : kDefaultChoice];
}

bool readCode(uint8_t& code, uint16_t* appliedMa) {
  uint8_t lo = 0, hi = 0;
  if (!read8(kRegIchgLo, lo)) return false;
  if (!read8(kRegIchgHi, hi)) return false;
  code = static_cast<uint8_t>(((lo >> 6) & 0x03) | ((hi & 0x0F) << 2));
  if (appliedMa != nullptr) *appliedMa = static_cast<uint16_t>(code) * kStepMa;
  return true;
}

bool applyMa(const uint16_t ma, uint16_t* appliedMaOut, uint8_t* codeOut) {
  const uint16_t clamped = std::clamp<uint16_t>(ma, kMinMa, kMaxMa);
  uint32_t code = clamped / kStepMa;
  if (code < 1) code = 1;  // code 0 is not a valid ICHG on this part
  if (code > kMaxCode) code = kMaxCode;
  const uint8_t want = static_cast<uint8_t>(code);

  // No pre-write read here any more. It existed to report `was=` (the code the part held before we
  // touched it); with that field gone, the read would be a wasted I2C transaction plus a second
  // failure path. updateBits() below performs the same read as its read-modify-write, and that read
  // is the presence test -- so a dead bus still cannot half-write the pair.
  if (!updateBits(kRegIchgLo, 0x03, 6, static_cast<uint8_t>(want & 0x03), "ICHG lo", ma)) return false;
  if (!updateBits(kRegIchgHi, 0x0F, 0, static_cast<uint8_t>((want >> 2) & 0x0F), "ICHG hi", ma)) return false;

  // Read back rather than assume: the setting's whole point is what the IC took, and
  // 500/1000/1500 are NOT representable (80 mA steps give 480/960/1440).
  uint8_t got = 0;
  uint16_t gotMa = 0;
  const bool readBackOk = readCode(got, &gotMa);
  if (appliedMaOut != nullptr) *appliedMaOut = readBackOk ? gotMa : static_cast<uint16_t>(want) * kStepMa;
  if (codeOut != nullptr) *codeOut = readBackOk ? got : want;

  const unsigned long setMa = static_cast<unsigned long>(ma);
  const unsigned long applied = static_cast<unsigned long>(readBackOk ? gotMa : want * static_cast<uint16_t>(kStepMa));
  if (!readBackOk) {
    LOG_ERR("PWR", "charger current: set=%lumA applied=%lumA UNCONFIRMED -- read-back failed (wrote code=%u)",
            setMa, applied, static_cast<unsigned>(want));
    return false;
  }
  if (got != want) {
    LOG_ERR("PWR", "charger current: set=%lumA applied=%lumA (code=%u) READ-BACK MISMATCH, wrote code=%u",
            setMa, applied, static_cast<unsigned>(got), static_cast<unsigned>(want));
    return false;
  }
  LOG_INF("PWR", "charger current: set=%lumA applied=%lumA (code=%u)", setMa, applied,
          static_cast<unsigned>(got));
  return true;
}

bool applyChoice(const uint8_t choice, uint16_t* appliedMaOut) {
  return applyMa(maForChoice(choice), appliedMaOut);
}

// ---- r117 BUCKET A: VREG 4400 mV + EN_TERM (the vendor's init defaults) --------------
// Same updateBits() read-modify-write as the ICHG path, for the same reason: REG0x04
// and REG0x14 carry other fields and a blind byte write would clear them.
namespace {
constexpr uint8_t kRegVregLo = 0x04;
constexpr uint8_t kRegVregHi = 0x05;
constexpr uint8_t kRegChgCtrl0 = 0x14;
constexpr uint16_t kVregStepMv = 10;
constexpr uint16_t kVregMinMv = 3840;  // CX25601N_VREG_MIN_MV
constexpr uint16_t kVregMaxMv = 4800;  // CX25601N_VREG_MAX_MV
}  // namespace

bool applyVendorVreg(uint16_t* appliedMvOut) {
  const uint16_t mv = std::clamp<uint16_t>(kVregMv, kVregMinMv, kVregMaxMv);
  uint32_t code = mv / kVregStepMv;  // 440 for 4400 mV
  if (!updateBits(kRegVregLo, 0x1F, 3, static_cast<uint8_t>(code & 0x1F), "VREG lo", mv)) return false;
  if (!updateBits(kRegVregHi, 0x0F, 0, static_cast<uint8_t>((code >> 5) & 0x0F), "VREG hi", mv)) return false;
  // EN_TERM. With the r163 E4 loop compiled in, the boot value is DISARMED (0), not the
  // vendor's init default of 1: the loop arms it the moment the pack is at/above 4400 mV
  // (ChargerPolicy.h), and disarmed is the correct state for any pack below the cut-off. It
  // is the strictly safer of the two boot states, because it cannot terminate a charge that
  // has not reached the cut-off. With CP_CHARGER_CONTROL=0 there is no loop to arm it, so the
  // vendor's default (1) is written and the pre-r163 behaviour is reproduced exactly.
  const uint8_t bootTerm = (CP_CHARGER_CONTROL != 0) ? 0 : 1;
  if (!updateBits(kRegChgCtrl0, 0x01, 2, bootTerm, "EN_TERM", mv)) return false;

  // Read back: the whole point is what the part holds, not what was written.
  uint8_t lo = 0, hi = 0;
  if (!read8(kRegVregLo, lo) || !read8(kRegVregHi, hi)) {
    LOG_ERR("PWR", "charger vreg: set=%umV UNCONFIRMED -- read-back failed", static_cast<unsigned>(mv));
    return false;
  }
  const uint32_t gotCode = static_cast<uint32_t>((lo >> 3) & 0x1F) | (static_cast<uint32_t>(hi & 0x0F) << 5);
  const uint16_t gotMv = static_cast<uint16_t>(gotCode * kVregStepMv);
  if (appliedMvOut != nullptr) *appliedMvOut = gotMv;
  if (gotMv != mv) {
    LOG_ERR("PWR", "charger vreg: set=%umV applied=%umV READ-BACK MISMATCH", static_cast<unsigned>(mv),
            static_cast<unsigned>(gotMv));
    return false;
  }
  LOG_INF("PWR", "charger vreg: applied=%umV, EN_TERM=%u (%s)", static_cast<unsigned>(gotMv),
          static_cast<unsigned>(bootTerm),
          (CP_CHARGER_CONTROL != 0) ? "disarmed at boot; the r163 E4 loop owns it"
                                    : "vendor cx25601n_init default (E4 loop compiled out)");
  return true;
}

}  // namespace chargercurrent

// ===========================================================================
// r163 E4 -- the runtime charger control loop (declared in ChargerCurrent.h;
// the decisions themselves live in src/system/ChargerPolicy.h and are host-tested).
//
// This block is the only place in the tree that changes a charger register for BEHAVIOUR.
// It is deliberately small: one sample, one plan, at most one plan's writes, one journal
// line per write. Every register it can reach is in `chargerpolicy::isDocumentedRegister()`
// and `journaledWrite()` below refuses anything else, so a future edit that tries to write,
// say, ITERM or the charge timer fails here instead of on the glass.
// ===========================================================================
namespace chargercontrol {
#if CP_CHARGER_CONTROL
namespace {

chargerpolicy::Machine g_machine;
bool g_started = false;
uint32_t g_nextDueMs = 0;
// uint32_t: the counter only has to be big enough never to wrap inside one boot's flash of
// charger writes. 16 bits was an arbitrary ceiling on a diagnostic that costs two bytes more.
uint32_t g_writes = 0;
bool g_uvloAnnounced = false;
const char* g_reason = "";

constexpr uint8_t kGaugeRegVoltage = 0x08;  // BQ27220 Voltage(), mV
constexpr uint8_t kGaugeRegCurrent = 0x0C;  // BQ27220 Current(), mA, signed
constexpr uint8_t kRegStatus1 = 0x1E;       // CX25601N CHG_STAT[4:3] VBUS_STAT[2:0]

// The gauge shares the charger's bus but not its address. Read only, never written by this
// file: the gauge is the observer, the charger is the actuator (§1 of the vendor doc).
bool gaugeAddr(uint8_t& addr) {
  if (!BoardConfig::isMetalioEInk4Plus()) return false;
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeAddr == 0) return false;
  addr = g.gaugeAddr;
  return true;
}

bool readGauge16(const uint8_t addr, const uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) != 2) return false;
  const uint8_t lo = static_cast<uint8_t>(Wire.read());
  const uint8_t hi = static_cast<uint8_t>(Wire.read());
  out = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
  return true;
}

// What one masked write did. Three outcomes, not two: "the register already held the target" is
// neither a failure nor a write, and the kick's two-step needs to tell them apart -- a kick whose
// EN_TERM half is already zero must still make its EN_CHG half.
enum class WriteOutcome : uint8_t { Failed, Unchanged, Wrote };

// THE ONE WRITE SITE. Read-modify-write so a neighbouring field in the same byte is never
// cleared, and NOTHING is written when the target value is already there -- which is what
// makes "one write per poll" a property of the code rather than a promise. The read is also
// the presence test: a part that does not answer at 0x6B leaves the register untouched and
// logs one ERR, and there is no retry inside this call.
WriteOutcome journaledWrite(const uint8_t reg, const uint8_t mask, const uint8_t value, const char* label,
                            const char* reason, const char* ctx) {
  if (!chargerpolicy::isDocumentedRegister(reg)) {
    // Unreachable through the policy's enum; kept because this is the function every future
    // charger write will go through, and a write outside the documented set must be a hard
    // refusal rather than a review comment.
    LOG_ERR("PWR", "charger-write REFUSED: REG0x%02X is not in the documented set", reg);
    return WriteOutcome::Failed;
  }
  uint8_t cur = 0;
  if (!chargercurrent::read8(reg, cur)) {
    LOG_ERR("PWR", "charger-write: REG0x%02X not written (%s: %s) -- no answer at 0x%02X; "
                   "the IC keeps what it had",
            reg, label, reason, chargercurrent::kAddr);
    return WriteOutcome::Failed;
  }
  const uint8_t next = static_cast<uint8_t>((cur & static_cast<uint8_t>(~mask)) | (value & mask));
  if (next == cur) return WriteOutcome::Unchanged;  // already in the target state: no I/O, not a write
  if (!chargercurrent::write8(reg, next)) {
    LOG_ERR("PWR", "charger-write: REG0x%02X write failed (%s: %s)", reg, label, reason);
    return WriteOutcome::Failed;
  }
  // Read back before claiming it: the register, not the return code, is what the part acts on.
  uint8_t back = 0;
  if (!chargercurrent::read8(reg, back)) {
    LOG_ERR("PWR", "charger-write: REG0x%02X written but UNCONFIRMED (read-back failed) "
                   "old=0x%02X new=0x%02X (%s: %s)",
            reg, cur, next, label, reason);
    return WriteOutcome::Failed;
  }
  if (back != next) {
    LOG_ERR("PWR", "charger-write: REG0x%02X READ-BACK MISMATCH old=0x%02X wrote=0x%02X read=0x%02X "
                   "(%s: %s)",
            reg, cur, next, back, label, reason);
    return WriteOutcome::Failed;
  }
  ++g_writes;
  // THE JOURNAL LINE. Register, old byte, new byte, the field changed, the reason, and the
  // measurement the decision was made from -- one line, greppable as `charger-write`.
  LOG_INF("PWR", "charger-write REG0x%02X old=0x%02X new=0x%02X %s reason=%s | %s", reg, cur, next,
          label, reason, ctx);
  return WriteOutcome::Wrote;
}

const char* statName(uint8_t stat) {
  switch (stat & 0x03) {
    case 0: return "NOT";
    case 1: return "CC";
    case 2: return "CV";
    case 3: return "TOPOFF";
  }
  return "?";
}

}  // namespace

const char* lastReason() { return g_reason; }
uint32_t writeCount() { return g_writes; }
#else
// Compiled out: no state, no journal, no Wire -- the pre-E4 behaviour exactly.
const char* lastReason() { return "charger control compiled out (CP_CHARGER_CONTROL=0)"; }
uint32_t writeCount() { return 0; }
#endif  // CP_CHARGER_CONTROL

bool tick(const uint32_t nowMs) {
#if !CP_CHARGER_CONTROL
  // Compiled out: no Wire traffic, no state, no journal -- the pre-E4 behaviour exactly.
  (void)nowMs;
  return false;
#else
  if (!BoardConfig::isMetalioEInk4Plus()) return false;
  if (g_started && static_cast<int32_t>(nowMs - g_nextDueMs) < 0) return false;
  g_started = true;

  chargerpolicy::Sample s;
  s.nowMs = nowMs;

  // ---- the charger's status (one read, two fields) --------------------------
  uint8_t status1 = 0;
  if (chargercurrent::read8(kRegStatus1, status1)) {
    s.ok = true;
    s.statKnown = true;
    s.chgStat = static_cast<uint8_t>((status1 >> 3) & 0x03);
    s.vbusKnown = true;
    s.vbusStat = static_cast<uint8_t>(status1 & 0x07);
  }

  // ---- the pack -----------------------------------------------------------
  uint8_t addr = 0;
  if (gaugeAddr(addr)) {
    uint16_t mv = 0;
    if (readGauge16(addr, kGaugeRegVoltage, mv) && mv >= chargerpolicy::kVbatPlausibleMv) {
      s.vbatKnown = true;
      s.vbatMv = mv;
    }
    uint16_t ma = 0;
    if (readGauge16(addr, kGaugeRegCurrent, ma)) {
      s.currentKnown = true;
      s.currentMa = static_cast<int16_t>(ma);
    }
  }

  const chargerpolicy::Plan plan = g_machine.step(s);
  g_reason = plan.reason;
  g_nextDueMs = nowMs + plan.nextPollMs;

  char ctx[96];
  std::snprintf(ctx, sizeof(ctx), "vbat=%s%umV stat=%s%s i=%s%dmA vbus=%s%u",
                s.vbatKnown ? "" : "?", s.vbatKnown ? s.vbatMv : 0u,
                s.statKnown ? "" : "?", s.statKnown ? statName(s.chgStat) : "unk",
                s.currentKnown ? "" : "?", s.currentKnown ? static_cast<int>(s.currentMa) : 0,
                s.vbusKnown ? "" : "?", s.vbusKnown ? static_cast<unsigned>(s.vbusStat) : 0u);

  // ---- UVLO: announce it, once per episode, and write nothing --------------
  // The policy detects the condition and refuses the vendor's `REG0x18 = 0x01`. This line is the
  // whole output of that decision, and it is what a glass session (or a support log) is judged by:
  // the condition, the pack voltage and the measurement it came from. One line per episode, on the
  // 0 -> 2 edge; the level going back to 0 re-arms it.
  if (plan.uvlo >= 2 && !g_uvloAnnounced) {
    g_uvloAnnounced = true;
    LOG_ERR("PWR", "uvlo-detected: no input and vbat at/below 3100mV -- BATFET NOT written "
                   "(deliberate; docs/engineering/R163_ITEM_E4.md §6) | %s",
            ctx);
  } else if (plan.uvlo == 0) {
    g_uvloAnnounced = false;
  }

  const auto wasWritten = [](const WriteOutcome o) { return o == WriteOutcome::Wrote; };
  bool wrote = false;
  switch (plan.action) {
    case chargerpolicy::Action::None:
      break;
    case chargerpolicy::Action::EnTermOn:
      wrote = wasWritten(journaledWrite(chargerpolicy::kRegEnTerm, chargerpolicy::kBitEnTerm,
                                        chargerpolicy::kBitEnTerm, "EN_TERM=1", plan.reason, ctx));
      break;
    case chargerpolicy::Action::EnTermOff:
      wrote = wasWritten(journaledWrite(chargerpolicy::kRegEnTerm, chargerpolicy::kBitEnTerm, 0x00,
                                        "EN_TERM=0", plan.reason, ctx));
      break;
    case chargerpolicy::Action::KickStep1:
      // Termination first (the vendor's order: a kick with termination armed re-terminates),
      // then EN_CHG off with HIZ cleared. If the first write FAILS the second is NOT made: the
      // part is left as it was, and the policy's pendingKickRestore_ makes the next poll re-assert
      // EN_CHG (which then writes nothing because it is already on). Unchanged is not a failure:
      // EN_TERM already 0 still needs its EN_CHG half.
      if (journaledWrite(chargerpolicy::kRegEnTerm, chargerpolicy::kBitEnTerm, 0x00, "EN_TERM=0",
                         plan.reason, ctx) != WriteOutcome::Failed) {
        wrote = wasWritten(journaledWrite(chargerpolicy::kRegEnChg,
                                          chargerpolicy::kBitEnChg | chargerpolicy::kBitHiz, 0x00,
                                          "EN_CHG=0,HIZ=0", plan.reason, ctx));
      }
      break;
    case chargerpolicy::Action::KickRestore:
      wrote = wasWritten(journaledWrite(chargerpolicy::kRegEnChg,
                                        chargerpolicy::kBitEnChg | chargerpolicy::kBitHiz,
                                        chargerpolicy::kBitEnChg, "EN_CHG=1,HIZ=0", plan.reason, ctx));
      break;
  }
  // True ONLY when a register byte actually changed (ChargerCurrent.h): a plan whose target is
  // already in the register, or one that names no change at all, performs no I/O and is not one.
  return wrote;
#endif
}

}  // namespace chargercontrol
