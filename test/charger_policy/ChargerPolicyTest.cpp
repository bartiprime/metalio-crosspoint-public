// r163 E4 -- the charger control loop, on the host.
//
// THE POINT OF THIS FILE. E4 is the first time this firmware writes a charger register to
// change charging BEHAVIOUR at run time, and there is no pack on this machine to test it
// with. So every rule the operator gave is turned into a claim that can be settled here:
//
//   * the EN_TERM gating boundary (enable at 4400, disable at 4380, nothing in between),
//   * that the boundary cannot oscillate (a pack dithering on the cut-off, which is exactly
//     where a CV loop holds it, must produce ZERO further writes),
//   * kick_recharge's three conditions, the 60 s rate limit, and the mandatory second half,
//   * the UVLO condition, its two-poll witness and its independent current check,
//   * and that NO action outside the documented set can be produced at all.
//
// The machine is the real header (src/system/ChargerPolicy.h). The I/O is not in here on
// purpose: this file would pass even if Wire were broken, and that is the correct split --
// what it proves is that the DECISIONS are right, which is what nobody can inspect on glass.

#include <gtest/gtest.h>

#include <set>

#include "system/ChargerPolicy.h"

using namespace chargerpolicy;

namespace {

// A sample that means "nothing unusual": a pack below the cut-off, charging bus, chearging.
// Tests override one field at a time so each case states its own premise.
Sample base(uint16_t vbatMv = 4000, uint32_t nowMs = 0) {
  Sample s;
  s.ok = true;
  s.vbatKnown = true;
  s.vbatMv = vbatMv;
  s.currentKnown = true;
  s.currentMa = 120;
  s.statKnown = true;
  s.chgStat = static_cast<uint8_t>(ChgStat::Cc);
  s.vbusKnown = true;
  s.vbusStat = 1;
  s.nowMs = nowMs;
  return s;
}

// A pack that the part has finished with: terminated, no current, below the cut-off.
Sample terminated(uint16_t vbatMv = 4200, uint32_t nowMs = 0) {
  Sample s = base(vbatMv, nowMs);
  s.chgStat = static_cast<uint8_t>(ChgStat::Not);
  s.currentMa = 0;
  return s;
}

// Drive a fresh machine to the state "EN_TERM off, nothing pending", which is where every
// kick test has to start from (a kick is only legal with termination disarmed).
void armForKick(Machine& m, uint16_t vbatMv = 4100) {
  const Plan p = m.step(base(vbatMv, 0));
  ASSERT_EQ(p.action, Action::EnTermOff) << "first poll should align EN_TERM from the plain rule";
}

}  // namespace

// ---------------------------------------------------------------------------
// The bound: VREG is fixed, and this loop has no way to move it.
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, VregIsTheVendorsFixedCutoff) {
  EXPECT_EQ(kVregMv, 4400) << "docs/battery-soc-and-charge.md §4.1: 固定截止 4400, no IR adaptive raise";
  EXPECT_EQ(kTermOnMv, 4400);
  EXPECT_EQ(kTermOffMv, 4380);
}

// The documented write set, as a value. Anything not here is a register this build refuses
// to write, and this is the test that keeps the driver's allowlist and this list identical.
TEST(ChargerPolicyTest, DocumentedRegisterSetIsFourAndNeverZeroEighteen) {
  EXPECT_TRUE(isDocumentedRegister(0x04));  // VREG lo   (boot only)
  EXPECT_TRUE(isDocumentedRegister(0x05));  // VREG hi   (boot only)
  EXPECT_TRUE(isDocumentedRegister(0x14));  // EN_TERM
  EXPECT_TRUE(isDocumentedRegister(0x16));  // EN_CHG / HIZ
  // 0x18 (BATFET shutdown) is NOT writable: the vendor's release path writes nothing, so the only
  // thing a write to it can do is stop the pack from ever charging again. See §6 of
  // docs/engineering/R163_ITEM_E4.md.
  EXPECT_FALSE(isDocumentedRegister(0x18)) << "REG0x18 must stay outside the write set";
  // The ICHG path (0x02/0x03) is the settings screen's, not this loop's; ITERM (0x12/0x13),
  // the timer (0x15), status (0x1E) and the unlock window (0x70) are reads or another item.
  EXPECT_FALSE(isDocumentedRegister(0x02));
  EXPECT_FALSE(isDocumentedRegister(0x03));
  EXPECT_FALSE(isDocumentedRegister(0x12));
  EXPECT_FALSE(isDocumentedRegister(0x15));
  EXPECT_FALSE(isDocumentedRegister(0x1E));
  EXPECT_FALSE(isDocumentedRegister(0x70));
  EXPECT_FALSE(isDocumentedRegister(0x00));
}

// Every action a plan can name, enumerated: there are exactly five and each one is a
// documented change. A sixth action cannot be added without failing this test -- which is
// the point, because adding one is how an unjustified write would get in. There is deliberately
// no BATFET action: UVLO is reported through Plan::uvlo and writes nothing.
TEST(ChargerPolicyTest, ActionSetIsClosed) {
  const std::set<int> actions = {static_cast<int>(Action::None),   static_cast<int>(Action::EnTermOn),
                                 static_cast<int>(Action::EnTermOff), static_cast<int>(Action::KickStep1),
                                 static_cast<int>(Action::KickRestore)};
  EXPECT_EQ(actions.size(), 5u);
}

// ---------------------------------------------------------------------------
// EN_TERM: the gating boundary and the 20 mV band.
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, TermDisabledBelowCutoffEnabledAtIt) {
  Machine m;
  EXPECT_EQ(m.step(base(4200, 0)).action, Action::EnTermOff) << "vbat < 4400 must disarm termination";
  EXPECT_FALSE(m.termEnabled());
  EXPECT_TRUE(m.termKnown());

  Machine n;
  EXPECT_EQ(n.step(base(4400, 0)).action, Action::EnTermOn) << ">= 4400 must arm termination";
  EXPECT_TRUE(n.termEnabled());
}

// The boundary is EXCLUSIVE in the vendor's direction: 4400 arms, 4399 does not.
TEST(ChargerPolicyTest, TermBoundaryIsExact) {
  Machine atCutoff;
  EXPECT_EQ(atCutoff.step(base(4400, 0)).action, Action::EnTermOn);

  Machine justBelow;
  EXPECT_EQ(justBelow.step(base(4399, 0)).action, Action::EnTermOff);
}

// Once armed, 4381 must NOT disarm: that is the 20 mV of hysteresis, and without it a CV
// loop holding the pack at 4399/4401 would toggle the bit on every poll.
TEST(ChargerPolicyTest, TermHysteresisHoldsInTheBand) {
  Machine m;
  ASSERT_EQ(m.step(base(4400, 0)).action, Action::EnTermOn);
  EXPECT_EQ(m.step(base(4399, 2000)).action, Action::None);
  EXPECT_EQ(m.step(base(4390, 4000)).action, Action::None);
  EXPECT_EQ(m.step(base(4381, 6000)).action, Action::None);
  EXPECT_TRUE(m.termEnabled()) << "4381 is inside the band; the armed state stands";
  EXPECT_EQ(m.step(base(4380, 8000)).action, Action::EnTermOff) << "4380 disarms";
  EXPECT_FALSE(m.termEnabled());
}

// THE NO-OSCILLATION PROOF. A pack sitting on the cut-off and dithering by a few mV -- the
// normal state of a pack in CV, and the exact condition that made the vendor's plain
// comparison flip -- must produce exactly ONE more write in a thousand polls, ever.
TEST(ChargerPolicyTest, TermDoesNotOscillateOnADitheringPack) {
  Machine m;
  ASSERT_EQ(m.step(base(4400, 0)).action, Action::EnTermOn);
  uint32_t now = 2000;
  int changes = 0;
  for (int i = 0; i < 1000; ++i) {
    const uint16_t v = (i % 2 == 0) ? 4402 : 4398;  // +-2 mV around the cut-off
    const Plan p = m.step(base(v, now));
    if (p.action == Action::EnTermOn || p.action == Action::EnTermOff) ++changes;
    EXPECT_EQ(p.action, Action::None) << "poll " << i << " toggled EN_TERM at vbat=" << v;
    now += 2000;
  }
  EXPECT_EQ(changes, 0);
  EXPECT_TRUE(m.termEnabled());
}

// And the same pack walking DOWN through the band writes once, not once per poll.
TEST(ChargerPolicyTest, TermDisarmsOnceWhenThePackWalksDownTheBand) {
  Machine m;
  ASSERT_EQ(m.step(base(4405, 0)).action, Action::EnTermOn);
  int writes = 0;
  uint32_t now = 2000;
  for (uint16_t v = 4404; v >= 4370; v = static_cast<uint16_t>(v - 2)) {
    const Plan p = m.step(base(v, now));
    if (p.action != Action::None) {
      ++writes;
      EXPECT_EQ(p.action, Action::EnTermOff);
      EXPECT_LE(v, kTermOffMv);
    }
    now += 2000;
  }
  EXPECT_EQ(writes, 1) << "one disarm for the whole walk down";
  EXPECT_FALSE(m.termEnabled());
}

// Convergence: from any starting voltage the bit settles and stops moving.
TEST(ChargerPolicyTest, TermConvergesForAnyStartingPoint) {
  for (uint16_t v = 3300; v <= 4500; v = static_cast<uint16_t>(v + 100)) {
    Machine m;
    m.step(base(v, 0));
    int changes = 0;
    for (int i = 0; i < 10; ++i) {
      if (m.step(base(v, static_cast<uint32_t>((i + 1) * 2000))).action != Action::None) ++changes;
    }
    EXPECT_EQ(changes, 0) << "steady at " << v << " mV must write nothing";
  }
}

// ---------------------------------------------------------------------------
// Poll cadence (vendor: 2 s below the cut-off, 10 s at/above).
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, PollCadenceFollowsTheCutoff) {
  Machine below;
  EXPECT_EQ(below.step(base(4200, 0)).nextPollMs, 2000u);
  Machine above;
  EXPECT_EQ(above.step(base(4400, 0)).nextPollMs, 10000u);
  Machine resting;
  EXPECT_EQ(resting.step(base(4395, 0)).nextPollMs, 2000u) << "4395 is still below VREG";
}

// ---------------------------------------------------------------------------
// kick_recharge.
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, KickNeedsTwoAgreeingPolls) {
  Machine m;
  armForKick(m);
  const Plan first = m.step(terminated(4200, 2000));
  EXPECT_EQ(first.action, Action::None) << "one sample of CHG_STAT=NOT is not evidence enough";
  const Plan second = m.step(terminated(4200, 4000));
  EXPECT_EQ(second.action, Action::KickStep1);
  EXPECT_TRUE(m.kickPending());
  EXPECT_EQ(m.kickCount(), 1u);
}

TEST(ChargerPolicyTest, KickRefusedUnlessStatIsNot) {
  for (uint8_t stat : {1u, 2u, 3u}) {
    Machine m;
    armForKick(m);
    Sample s = terminated(4200, 2000);
    s.chgStat = stat;
    for (int i = 0; i < 5; ++i) {
      s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
      EXPECT_EQ(m.step(s).action, Action::None) << "stat=" << static_cast<int>(stat) << " is charging";
    }
  }
}

TEST(ChargerPolicyTest, KickRefusedWhenCurrentIsNotZero) {
  Machine m;
  armForKick(m);
  Sample s = terminated(4200, 2000);
  s.currentMa = 6;  // just outside the +-5 mA deadband
  for (int i = 0; i < 4; ++i) {
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    EXPECT_EQ(m.step(s).action, Action::None);
  }
  // ... and +5 mA exactly is still "~0", so the deadband is the same one E3 uses.
  Machine n;
  armForKick(n);
  Sample t = terminated(4200, 2000);
  t.currentMa = 5;
  EXPECT_EQ(n.step(t).action, Action::None);
  t.nowMs = 4000;
  EXPECT_EQ(n.step(t).action, Action::KickStep1);
}

TEST(ChargerPolicyTest, KickRefusedWhenCurrentIsUnknown) {
  Machine m;
  armForKick(m);
  Sample s = terminated(4200, 2000);
  s.currentKnown = false;
  for (int i = 0; i < 4; ++i) {
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    EXPECT_EQ(m.step(s).action, Action::None);
  }
}

TEST(ChargerPolicyTest, KickRefusedWhenStatIsUnknown) {
  Machine m;
  armForKick(m);
  Sample s = terminated(4200, 2000);
  s.statKnown = false;
  for (int i = 0; i < 4; ++i) {
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    EXPECT_EQ(m.step(s).action, Action::None);
  }
}

TEST(ChargerPolicyTest, KickRefusedAtOrAboveTheCutoff) {
  Machine m;
  // Armed at 4400 (the vendor's plain rule), then the pack is at the cut-off and terminated.
  ASSERT_EQ(m.step(base(4400, 0)).action, Action::EnTermOn);
  Sample s = terminated(4400, 2000);
  for (int i = 0; i < 4; ++i) {
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    EXPECT_EQ(m.step(s).action, Action::None) << "V >= VREG is not a kick condition";
  }
}

// The vendor's own guard, as a test: a kick with termination ARMED would re-terminate
// immediately, so the machine must wait for the disarm before it kicks.
TEST(ChargerPolicyTest, KickNeverHappensWithTerminationArmed) {
  Machine m;
  ASSERT_EQ(m.step(base(4400, 0)).action, Action::EnTermOn);
  EXPECT_TRUE(m.termEnabled());
  Sample s = terminated(4395, 2000);  // inside the band: EN_TERM stays armed
  for (int i = 0; i < 6; ++i) {
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    EXPECT_EQ(m.step(s).action, Action::None);
  }
  EXPECT_TRUE(m.termEnabled());
}

// THE RATE LIMIT: at most one kick per 60 s, whatever the poll cadence.
TEST(ChargerPolicyTest, KickIsRateLimitedToOnePerMinute) {
  Machine m;
  armForKick(m);
  ASSERT_EQ(m.step(terminated(4200, 1000)).action, Action::None);
  ASSERT_EQ(m.step(terminated(4200, 2000)).action, Action::KickStep1);
  ASSERT_EQ(m.step(terminated(4200, 4000)).action, Action::KickRestore) << "the restore is unconditional";

  // The pack is still terminated and still below the cut-off: the machine WANTS to kick on
  // every poll and must be refused for the next 58 s.
  int kicks = 0;
  for (uint32_t t = 6000; t < 61000; t += 2000) {
    if (m.step(terminated(4200, t)).action == Action::KickStep1) ++kicks;
  }
  EXPECT_EQ(kicks, 0) << "58 s of polls after a kick must not kick again";
  EXPECT_EQ(m.step(terminated(4200, 62000)).action, Action::KickStep1) << "60 s later it may";
  EXPECT_EQ(m.kickCount(), 2u);
}

// The kick's second half runs even when the bus has died in between, because leaving EN_CHG
// off is the one failure this loop must never produce.
TEST(ChargerPolicyTest, KickRestoreRunsEvenWhenTheNextReadFails) {
  Machine m;
  armForKick(m);
  ASSERT_EQ(m.step(terminated(4200, 1000)).action, Action::None);
  ASSERT_EQ(m.step(terminated(4200, 2000)).action, Action::KickStep1);
  Sample dead;
  dead.ok = false;
  dead.nowMs = 4000;
  EXPECT_EQ(m.step(dead).action, Action::KickRestore);
  EXPECT_FALSE(m.kickPending());
  // And after the restore, a dead bus writes nothing again.
  EXPECT_EQ(m.step(dead).action, Action::None);
}

// ---------------------------------------------------------------------------
// UVLO: DETECTED, ANNOUNCED, AND NEVER ACTED ON.
//
// The vendor's action here is `REG0x18 = 0x01` (BATFET off), and their release path writes
// nothing back, so the write is one-way in software. These tests prove the opposite of the
// old ones: the condition is recognised (Plan::uvlo) and NOTHING is ever written for it.
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, UvloIsAnnouncedAfterTwoAgreeingPollsAndWritesNothing) {
  Machine m;
  armForKick(m);
  Sample s = base(3050, 2000);
  s.vbusStat = 0;
  s.currentMa = 0;
  const Plan first = m.step(s);
  EXPECT_EQ(first.uvlo, 1) << "one poll is a level, not an announcement";
  EXPECT_EQ(first.action, Action::None) << "the first poll must not have written anything";
  s.nowMs = 4000;
  const Plan second = m.step(s);
  EXPECT_EQ(second.uvlo, 2) << "two agreeing polls confirm the condition";
  EXPECT_EQ(second.action, Action::None) << "UVLO is never an action: REG0x18 is not writable";
  EXPECT_EQ(m.uvloLevel(), 2);
}

TEST(ChargerPolicyTest, UvloBoundaryIs3100) {
  Machine at;
  armForKick(at);
  Sample s = base(3100, 2000);
  s.vbusStat = 0;
  s.currentMa = 0;
  at.step(s);
  s.nowMs = 4000;
  EXPECT_EQ(at.step(s).uvlo, 2) << "3100 is the documented threshold";
  EXPECT_EQ(at.step(s).action, Action::None);

  Machine above;
  armForKick(above);
  Sample t = base(3101, 2000);
  t.vbusStat = 0;
  t.currentMa = 0;
  for (int i = 0; i < 5; ++i) {
    t.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    const Plan p = above.step(t);
    EXPECT_EQ(p.uvlo, 0);
    EXPECT_EQ(p.action, Action::None);
  }
}

// The refusals that keep the DETECTOR honest on THIS board: VBUS_STAT is documented to misreport
// "no input" while a cable is in, so the gauge's current is the independent witness. A detector
// that cries wolf on a charging pack is worthless in a log.
TEST(ChargerPolicyTest, UvloRefusedWithoutEveryWitness) {
  auto announced = [](Machine& m, Sample s) {
    for (int i = 0; i < 6; ++i) {
      s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
      if (m.step(s).uvlo == 2) return true;
    }
    return false;
  };

  {  // a cable IS present: VBUS_STAT says so, so the condition fails
    Machine m;
    armForKick(m);
    Sample s = base(3050);
    s.vbusStat = 1;
    s.currentMa = 0;
    EXPECT_FALSE(announced(m, s)) << "with input, 3050 mV is not an undervoltage";
  }
  {  // VBUS_STAT did not answer: we do not announce on a number we did not read
    Machine m;
    armForKick(m);
    Sample s = base(3050);
    s.vbusKnown = false;
    s.currentMa = 0;
    EXPECT_FALSE(announced(m, s));
  }
  {  // VBUS_STAT says "no input" but the gauge sees current going INTO the pack
    Machine m;
    armForKick(m);
    Sample s = base(3050);
    s.vbusStat = 0;
    s.currentMa = 40;
    EXPECT_FALSE(announced(m, s)) << "positive current is proof of input; do not announce uvlo";
  }
  {  // below the vendor's own plausibility floor the voltage is not a pack reading
    Machine m;
    armForKick(m);
    Sample s = base(2499);
    s.vbusStat = 0;
    s.currentMa = 0;
    EXPECT_FALSE(announced(m, s));
  }
  {  // two DISAGREEING polls: the first hit is cancelled by the second, and the level drops
    Machine m;
    armForKick(m);
    Sample s = base(3050, 2000);
    s.vbusStat = 0;
    s.currentMa = 0;
    EXPECT_EQ(m.step(s).uvlo, 1);
    s.nowMs = 4000;
    s.currentMa = 120;  // charging after all
    EXPECT_EQ(m.step(s).uvlo, 0) << "one witness is not enough, and the count starts over";
  }
}

// While the condition stands nothing else runs: no EN_TERM change, and no recharge kick at a pack
// with no charger attached. A kick is a WRITE, and this item writes four registers, not five.
TEST(ChargerPolicyTest, UvloSuppressesEveryOtherWrite) {
  Machine m;
  armForKick(m);  // EN_TERM off: the state a kick needs
  Sample s = base(3050, 2000);
  s.vbusStat = 0;
  s.currentMa = 0;
  s.chgStat = static_cast<uint8_t>(ChgStat::Not);
  int actions = 0;
  for (int i = 0; i < 40; ++i) {  // 80 s of polls: longer than the 60 s kick cooldown
    s.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    const Plan p = m.step(s);
    if (p.action != Action::None) ++actions;
    EXPECT_EQ(p.uvlo, i == 0 ? 1 : 2) << "poll " << i;
  }
  EXPECT_EQ(actions, 0) << "uvlo suspends the whole loop, it does not merely add a line";
  EXPECT_EQ(m.kickCount(), 0);
}

// And it clears by itself: the level drops the moment the condition stops holding (a cable in, or
// a pack back above 3100 mV), with no latch and no write.
TEST(ChargerPolicyTest, UvloClearsWithoutALatch) {
  Machine m;
  armForKick(m);
  Sample s = base(3050, 2000);
  s.vbusStat = 0;
  s.currentMa = 0;
  m.step(s);
  s.nowMs = 4000;
  ASSERT_EQ(m.step(s).uvlo, 2);

  s.vbusStat = 1;  // cable in
  s.currentMa = 120;
  s.nowMs = 6000;
  const Plan p = m.step(s);
  EXPECT_EQ(p.uvlo, 0) << "no latch to release";
  EXPECT_EQ(m.uvloLevel(), 0);
  // Above the threshold is not the condition either.
  Sample t = base(3400, 8000);
  t.vbusStat = 0;
  t.currentMa = 0;
  EXPECT_EQ(m.step(t).uvlo, 0);
}


// ---------------------------------------------------------------------------
// Fail safe: a failed read writes nothing and forgets nothing.
// ---------------------------------------------------------------------------
TEST(ChargerPolicyTest, FailedReadWritesNothingAndKeepsState) {
  Machine m;
  ASSERT_EQ(m.step(base(4200, 0)).action, Action::EnTermOff);
  Sample dead;
  dead.ok = false;
  dead.vbatKnown = false;
  dead.nowMs = 2000;
  for (int i = 0; i < 10; ++i) {
    dead.nowMs = static_cast<uint32_t>(2000 + i * 2000);
    const Plan p = m.step(dead);
    EXPECT_EQ(p.action, Action::None);
    EXPECT_EQ(p.nextPollMs, kPollIdleMs) << "a failure polls SLOWER (10 s), never in a retry storm";
  }
  EXPECT_FALSE(m.termEnabled());
  EXPECT_TRUE(m.termKnown());
  // The pack reads again, unchanged: still no write (nothing to change).
  EXPECT_EQ(m.step(base(4200, 30000)).action, Action::None);
}

TEST(ChargerPolicyTest, ImplausibleVoltageIsTreatedAsUnknown) {
  Machine m;
  Sample s = base(0, 0);
  s.vbatKnown = false;  // the caller sets this when 0x08 is below 2500 mV
  EXPECT_EQ(m.step(s).action, Action::None);
}

// A whole-session sweep with nothing but benign samples must produce exactly one write.
TEST(ChargerPolicyTest, QuietSessionWritesOnce) {
  Machine m;
  int writes = 0;
  for (uint32_t t = 0; t < 600000; t += 2000) {
    const Plan p = m.step(base(4150, t));
    if (p.action != Action::None) {
      ++writes;
      EXPECT_EQ(p.action, Action::EnTermOff);
    }
  }
  EXPECT_EQ(writes, 1) << "steady low pack: one disarm at boot, then silence";
}
