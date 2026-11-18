#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// r163 E4 -- THE CHARGER'S CONTROL LOOP, as arithmetic, with NO I/O.
//
// WHAT THIS IS. The vendor's runtime charger policy (`docs/battery-soc-and-charge.md` §4.2,
// `cx25601n.cc` `charger_vreg_task` + `kick_recharge`), written against OUR driver:
//
//   * VREG is FIXED at 4400 mV. It is NOT in this loop and never will be: the vendor's
//     IR-adaptive raising is what made CC and CV fight each other, and §4.1 says so in one
//     line ("固定截止，不做 IR 自适应抬压"). VREG is written ONCE per boot by
//     `chargercurrent::applyVendorVreg()` and this file cannot touch it.
//   * `EN_TERM` (REG0x14 bit 2) is DISABLED while Vbat < VREG and ENABLED at/above it, so a
//     pack that has not reached the cut-off can never be terminated by the ITERM comparator.
//   * `kick_recharge`: `CHG_STAT = NOT` + current ~ 0 + V < VREG means the part ended a
//     charge and will not restart on its own; toggle `EN_CHG` with `EN_TERM` OFF to force a
//     new round. Never with `EN_TERM` enabled (the vendor's own rule, comment on
//     cx25601n.cc:134-135 -- a kick with termination armed immediately re-terminates).
//   * Poll 2 s below the cut-off, 10 s at/above.
//   * UVLO: no VBUS + Vbat <= 3100 mV is DETECTED and ANNOUNCED, and NOT ACTED ON. The vendor's
//     action is `REG0x18 = 0x01` (BATFET shutdown), and that write is one-way in software: their
//     release branch (cx25601n.cc:197-199) only clears a RAM flag when the pack is back above
//     3200 mV, and nothing else in their tree writes bit 0 of 0x18. Whether the part restores the
//     FET on adapter insertion or POR is a hardware assumption nobody has tested, and its failure
//     mode is "the pack never charges again and no code path can fix it". A control that can
//     permanently stop charging is worse than no control, so this build refuses it. See
//     docs/engineering/R163_ITEM_E4.md §6.
//
// WHY IT IS A HEADER AND NOT A TASK. Every decision below is checkable from a sequence of
// samples, so it is checked on the host (test/charger_policy) before it can ever reach a
// pack. `ChargerCurrent.cpp` is the only thing that owns Wire: it takes the sample, asks this
// machine for ONE plan, and performs at most the writes that plan names.
//
// ---------------------------------------------------------------------------
// THE FOUR BOUNDS THIS MACHINE KEEPS, and why each one is here:
//
// 1. A PLAN NAMES AT MOST ONE KIND OF CHANGE. `step()` returns one `Action`; `KickStep1` is
//    the only action that touches two registers, and it touches them for the one documented
//    reason (EN_TERM off, then EN_CHG off). Nothing else composes. A bit therefore cannot be
//    toggled twice inside one poll interval by construction, not by care.
//
// 2. EN_TERM HAS 20 mV OF HYSTERESIS (4400 to enable, 4380 to disable). The vendor's rule is
//    a bare comparison and their poll is 2 s, so a pack resting on the cut-off -- which is
//    exactly what a charged pack does, because the CV loop regulates it there -- would flip
//    the bit on alternating polls forever. 20 mV is the same width as the gauge's own
//    full/exit band (4300/4280, §3.2) and is the smallest band that cannot be crossed
//    by the part's own CV ripple.
//
// 3. KICK AND UVLO NEED TWO CONSECUTIVE AGREEING SAMPLES, and a kick is rate-limited to one
//    per 60 s. The vendor kicks on a single sample of their 2 s poll and has no rate limit;
//    one flaky `CHG_STAT` read or one noisy current sample becomes a charge cycle there. A
//    60 s floor is still four times gentler than the vendor's own termination behaviour and
//    cannot oscillate: 60 s >> any poll interval.
//
// 4. NOTHING IS WRITTEN ON A FAILED READ. A sample whose status/gauge reads failed yields
//    `Action::None`; the register keeps whatever it had. There is no retry inside the step
//    and no escalation -- the caller's next poll is the retry, and its interval is unchanged
//    by the failure.
// ---------------------------------------------------------------------------

namespace chargerpolicy {

// ---- the vendor's numbers (docs/battery-soc-and-charge.md §4.2, cx25601n.cc:43, :162-210) --
constexpr uint16_t kVregMv = 4400;          // fixed cut-off. Never raised, never lowered.
constexpr uint16_t kTermHystMv = 20;        // OUR hysteresis (see bound 2): off at <= 4380
constexpr uint16_t kTermOnMv = kVregMv;                     // enable at >= 4400
constexpr uint16_t kTermOffMv = kVregMv - kTermHystMv;      // disable at <= 4380

constexpr uint32_t kPollCutoffMs = 2000;    // below the cut-off
constexpr uint32_t kPollIdleMs = 10000;     // at/above it

constexpr int16_t kKickCurrentMa = 5;       // "current ~ 0": the same +-5 mA deadband E3 uses
constexpr uint32_t kKickCooldownMs = 60000; // OUR rate limit (see bound 3)

constexpr uint16_t kUvShutdownMv = 3100;    // <= this, with no input -> the UVLO condition
constexpr uint16_t kVbatPlausibleMv = 2500; // below this the reading is not a pack (vendor :184)

// Consecutive agreeing polls required before a destructive/expensive action.
constexpr uint8_t kAgreePolls = 2;

// ---- CHG_STAT (REG0x1E[4:3], cx25601n.h:41-44) ------------------------------
enum class ChgStat : uint8_t { Not = 0, Cc = 1, Cv = 2, TopOff = 3 };

// ---- THE DOCUMENTED WRITE SET ----------------------------------------------
// Register -> the ONE field this batch is allowed to change there. Anything not in this list is a
// register this build does not write, and `ChargerCurrent.cpp` refuses it at the one write site.
//
// `REG0x18` IS NOT IN THIS SET, deliberately. It is the vendor's BATFET-shutdown byte, and it is
// one-way in software (see the header comment): once written, nothing in this firmware can bring
// the FET back. A register that can only be written is not a control, so it is not writable.
enum class Reg : uint8_t {
  VregLo = 0x04,   // REG0x04[7:3] VREG code[4:0]   -- boot only, fixed 4400
  VregHi = 0x05,   // REG0x05[3:0] VREG code[8:5]   -- boot only
  ChgCtrl0 = 0x14, // REG0x14[2]   EN_TERM
  ChgCtrl1 = 0x16, // REG0x16[5]   EN_CHG      [4] HIZ (cleared, i.e. 0, only if it is 1)
};

// The bits this build may change, named once so the .cpp and the tests cannot disagree.
constexpr uint8_t kRegEnTerm = static_cast<uint8_t>(Reg::ChgCtrl0);
constexpr uint8_t kBitEnTerm = 1u << 2;   // REG0x14 bit 2
constexpr uint8_t kRegEnChg = static_cast<uint8_t>(Reg::ChgCtrl1);
constexpr uint8_t kBitEnChg = 1u << 5;    // REG0x16 bit 5
constexpr uint8_t kBitHiz = 1u << 4;      // REG0x16 bit 4 -- must be 0 for EN_CHG to act

// Compile-time proof of the allowlist, usable from a test as well as from the driver: the four
// registers above are the whole set, and the two VREG bytes are boot-only.
constexpr bool isDocumentedRegister(uint8_t reg) {
  return reg == 0x04 || reg == 0x05 || reg == 0x14 || reg == 0x16;
}

// ---- what one poll may ask for ---------------------------------------------
enum class Action : uint8_t {
  None = 0,
  EnTermOn,      // REG0x14 bit2 := 1
  EnTermOff,     // REG0x14 bit2 := 0
  KickStep1,     // REG0x14 bit2 := 0 AND REG0x16 bit5 := 0  (one logical step, two registers)
  KickRestore,   // REG0x16 bit5 := 1 (bit4 HIZ := 0 if set) -- the kick's mandatory second half
};

struct Sample {
  bool ok = false;            // the charger answered its status read
  bool vbatKnown = false;     // gauge 0x08 answered and is plausible
  uint16_t vbatMv = 0;
  bool currentKnown = false;  // gauge 0x0C answered
  int16_t currentMa = 0;      // + into the pack, - out of it
  bool statKnown = false;     // CHG_STAT decoded
  uint8_t chgStat = 0;        // 0..3 (ChgStat)
  bool vbusKnown = false;     // VBUS_STAT decoded (unreliable on this board -- §4.1)
  uint8_t vbusStat = 0;       // 0 = "no input"
  uint32_t nowMs = 0;         // monotonic clock, for the kick's rate limit
};

struct Plan {
  Action action = Action::None;
  const char* reason = "no change";
  uint32_t nextPollMs = kPollIdleMs;
  // The UVLO condition, as a level: 0 = not present, 1 = seen once (awaiting the second agreeing
  // poll), 2 = confirmed. It names no change -- nothing is written for it -- but the driver
  // announces the 0 -> 2 edge once per episode, and that line is the whole output of the detector.
  uint8_t uvlo = 0;
};

// ---------------------------------------------------------------------------
// The machine. One instance per boot, in the driver's file, never persisted: every field
// below is a claim about a measurement, and a claim restored from NVS would outlive the
// evidence for it (the same rule as the gauge's FullCharge).
// ---------------------------------------------------------------------------
class Machine {
 public:
  Plan step(const Sample& s) {
    Plan p;

    // ---- the kick's second half, ALWAYS, and before anything else -----------
    // This is deliberately ABOVE the failed-read guard below: once EN_CHG has been turned
    // off it is turned back on at the very next poll whatever the inputs say, including a
    // dead bus. Any other placement would let one bad read at the wrong moment leave
    // charging disabled until reboot -- the one failure this loop must not have.
    if (pendingKickRestore_) {
      pendingKickRestore_ = false;
      p.action = Action::KickRestore;
      p.reason = "kick step 2: EN_CHG back on after the recharge attempt";
      p.nextPollMs = (s.vbatKnown && s.vbatMv < kVregMv) ? kPollCutoffMs : kPollIdleMs;
      return p;
    }

    // BOUND 4: a failed status read or an unreadable/invalid pack voltage -> no writes at
    // all. The cadence drops to the IDLE interval, so a dead bus costs 10 s per attempt
    // rather than 2 s: this is a poll loop, not a retry loop, and it must not storm.
    if (!s.ok || !s.vbatKnown) {
      p.reason = s.ok ? "vbat unknown" : "charger did not answer";
      p.nextPollMs = kPollIdleMs;
      return p;
    }

    // ---- UVLO: DETECTED AND ANNOUNCED, NEVER ACTED ON ----------------------
    // The vendor writes `REG0x18 = 0x01` here (BATFET off, cx25601n.cc:402-411). That bit is
    // one-way in software -- their own release path (:197-199) writes nothing -- so this build
    // detects the condition, raises Plan::uvlo for the journal, and leaves the part alone.
    //
    // The condition is still the vendor's (`VBUS_STAT == 0 && VBAT <= 3100`), with the two
    // witnesses the picket needs: VBUS_STAT is the ONE register this board misreports (§4.1,
    // DPDM off, so it reads 0 even with a cable), so the gauge's current must not be flowing
    // INTO the pack, the vendor's own 2500 mV plausibility floor applies, and two polls must
    // agree. While the condition stands nothing else runs either -- no EN_TERM change, no kick
    // at a pack with no charger on it -- which is what the vendor's latch did, without the latch.
    const bool uvCondition = s.vbatMv >= kVbatPlausibleMv && s.vbatMv <= kUvShutdownMv &&
                             s.vbusKnown && s.vbusStat == 0 &&
                             (!s.currentKnown || s.currentMa <= kKickCurrentMa);
    if (uvCondition) {
      if (uvHits_ < kAgreePolls) ++uvHits_;
      p.uvlo = uvHits_ >= kAgreePolls ? 2 : 1;
      p.reason = p.uvlo == 2 ? "uvlo: no input and vbat at/below 3100mV (BATFET not written)"
                             : "uvlo pending (awaiting a second agreeing poll)";
      p.nextPollMs = kPollIdleMs;
      return p;
    }
    uvHits_ = 0;

    const bool vbatBelow = s.vbatMv < kVregMv;

    // ---- EN_TERM, with the 20 mV band (bound 2) ---------------------------
    // The FIRST poll has no history to be hysteretic about, so it applies the vendor's plain
    // rule; every poll after it uses the band. A pack that walks through 4380..4400 in one
    // step changes the bit once, and a pack that hovers there changes it never.
    bool wantTerm;
    bool haveWant = true;
    if (enTerm_ == TermState::Unknown) {
      wantTerm = (s.vbatMv >= kTermOnMv);
    } else if (s.vbatMv >= kTermOnMv) {
      wantTerm = true;
    } else if (s.vbatMv <= kTermOffMv) {
      wantTerm = false;
    } else {
      haveWant = false;
      wantTerm = (enTerm_ == TermState::On);
    }
    if (haveWant) {
      const bool nowOn = (enTerm_ == TermState::On);
      if (enTerm_ == TermState::Unknown || wantTerm != nowOn) {
        enTerm_ = wantTerm ? TermState::On : TermState::Off;
        p.action = wantTerm ? Action::EnTermOn : Action::EnTermOff;
        p.reason = wantTerm ? "vbat at/above VREG 4400mV -> termination allowed"
                            : "vbat below VREG 4400mV -> termination disabled";
        p.nextPollMs = vbatBelow ? kPollCutoffMs : kPollIdleMs;
        return p;
      }
    }

    // ---- kick_recharge (vendor §4.2, cx25601n.cc:203-207) ------------------
    // CHG_STAT = NOT, current ~ 0, V < VREG, and -- the vendor's own guard -- EN_TERM off.
    // Two agreeing polls (bound 3) so one flaky read cannot start a charge cycle, and one
    // kick per 60 s so a pack that will not come back up cannot be kicked every poll.
    const bool kickCondition = s.statKnown && s.chgStat == static_cast<uint8_t>(ChgStat::Not) &&
                               s.currentKnown && s.currentMa <= kKickCurrentMa && vbatBelow &&
                               enTerm_ == TermState::Off;
    if (kickCondition) {
      if (kickHits_ < kAgreePolls) ++kickHits_;
    } else {
      kickHits_ = 0;
    }
    if (kickCondition && kickHits_ >= kAgreePolls && cooldownElapsed(s.nowMs)) {
      kickHits_ = 0;
      haveKicked_ = true;
      lastKickMs_ = s.nowMs;
      ++kickCount_;
      pendingKickRestore_ = true;
      enTerm_ = TermState::Off;  // the step writes EN_TERM off; the register is now off
      p.action = Action::KickStep1;
      p.reason = "recharge kick: CHG_STAT=NOT, current~0, vbat below VREG";
      p.nextPollMs = kPollCutoffMs;
      return p;
    }
    if (kickCondition && !cooldownElapsed(s.nowMs)) {
      p.reason = "kick suppressed by the 60 s rate limit";
      p.nextPollMs = kPollCutoffMs;
      return p;
    }

    p.reason = "no change";
    p.nextPollMs = vbatBelow ? kPollCutoffMs : kPollIdleMs;
    return p;
  }

  // Read-only state, for the journal line and for the host tests.
  bool termEnabled() const { return enTerm_ == TermState::On; }
  bool termKnown() const { return enTerm_ != TermState::Unknown; }
  bool kickPending() const { return pendingKickRestore_; }
  uint16_t kickCount() const { return kickCount_; }
  // The UVLO condition, last poll's verdict: 0 not present, 1 seen once, 2 confirmed. Detected,
  // never acted on -- there is no latch to query, because nothing is latched.
  uint8_t uvloLevel() const { return uvHits_ >= kAgreePolls ? 2 : (uvHits_ != 0 ? 1 : 0); }

 private:
  enum class TermState : uint8_t { Unknown, Off, On };

  bool cooldownElapsed(uint32_t nowMs) const {
    if (!haveKicked_) return true;
    return static_cast<uint32_t>(nowMs - lastKickMs_) >= kKickCooldownMs;
  }

  TermState enTerm_ = TermState::Unknown;
  bool pendingKickRestore_ = false;
  bool haveKicked_ = false;
  uint32_t lastKickMs_ = 0;
  uint16_t kickCount_ = 0;
  uint8_t uvHits_ = 0;
  uint8_t kickHits_ = 0;
};

}  // namespace chargerpolicy
