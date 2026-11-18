// r163 E6 -- THE POWER-NEED ALGEBRA, on the host.
//
// The header under test is src/system/PowerNeed.h: it owns no I/O, no task and no
// clock, so the device's behaviour is these counters plus the caller passing
// millis() in. Everything asserted here is what runs on the board.
//
// What is covered, and why each one is a real failure mode:
//   * acquire/release BALANCE -- the hold is a count, not a flag, so a nested
//     acquire cannot be ended by an inner release;
//   * HARD vs SOFT on the inactivity clock -- a hard release must restart it (a
//     session that ends must not sleep the device at once), a soft release must
//     not (its whole definition is that it is inert);
//   * DOUBLE RELEASE -- must be counted and clamped, never wrapped to 255;
//   * LEAK -- an aged hold is announced ONCE per excursion, is visible in the
//     report, and re-announces only after a release/re-acquire;
//   * REBASE -- a standby spans wall clock, so ages must restart at the resume or
//     every long park would announce a leak that never happened;
//   * the charge policy's truth table (item 3.5), which is one constexpr over the
//     persisted setting and the pack's own charging verdict.

#include <gtest/gtest.h>

#include <system/ChargeIdlePolicy.h>
#include <system/PowerNeed.h>

namespace {

using powerneed::Hold;
using powerneed::Need;
using powerneed::Tracker;

constexpr unsigned long kMinute = 60UL * 1000UL;

TEST(PowerNeed, HardAndSoftClassificationIsTheVendors) {
  // The vendor's own three: UsbVirtualDisk and StandbyInhibit block the INACTIVITY
  // standby; ForegroundNetwork does not.
  EXPECT_EQ(powerneed::holdOf(Need::UsbVirtualDisk), Hold::Hard);
  EXPECT_EQ(powerneed::holdOf(Need::StandbyInhibit), Hold::Hard);
  EXPECT_EQ(powerneed::holdOf(Need::ChargerHost), Hold::Hard);
  EXPECT_EQ(powerneed::holdOf(Need::ForegroundNetwork), Hold::Soft);
}

TEST(PowerNeed, AcquireReleaseBalanceIsACount) {
  Tracker t;
  EXPECT_FALSE(t.held(Need::StandbyInhibit));

  t.acquire(Need::StandbyInhibit, 1000);
  t.acquire(Need::StandbyInhibit, 2000);  // nested: two holders
  EXPECT_EQ(t.count(Need::StandbyInhibit), 2);

  EXPECT_TRUE(t.release(Need::StandbyInhibit, 3000));
  EXPECT_TRUE(t.held(Need::StandbyInhibit)) << "one release must not end a nested hold";

  EXPECT_TRUE(t.release(Need::StandbyInhibit, 4000));
  EXPECT_FALSE(t.held(Need::StandbyInhibit));

  const powerneed::Report r = t.report(4000);
  EXPECT_EQ(r.status[static_cast<int>(Need::StandbyInhibit)].acquiredTotal, 2u);
  EXPECT_EQ(r.status[static_cast<int>(Need::StandbyInhibit)].releasedTotal, 2u);
  EXPECT_EQ(r.doubleReleases, 0u);
}

TEST(PowerNeed, HardNeedBlocksAutomaticStandbyAndSoftDoesNot) {
  Tracker t;
  EXPECT_FALSE(t.blocksAutoStandby());

  t.acquire(Need::ForegroundNetwork, 0);
  EXPECT_TRUE(t.anyHeld()) << "a soft need is still a hold: it is visible";
  EXPECT_FALSE(t.blocksAutoStandby()) << "the vendor's ForegroundNetwork must not prevent standby";

  t.acquire(Need::UsbVirtualDisk, 0);
  EXPECT_TRUE(t.blocksAutoStandby());
  EXPECT_TRUE(t.report(0).blocksAutoStandby);

  t.release(Need::UsbVirtualDisk, 10);
  EXPECT_FALSE(t.blocksAutoStandby());
  t.release(Need::ForegroundNetwork, 10);
  EXPECT_FALSE(t.anyHeld());
}

TEST(PowerNeed, HardReleaseRefreshesTheIdleClockAndSoftReleaseDoesNot) {
  Tracker t;
  t.acquire(Need::UsbVirtualDisk, 0);
  EXPECT_FALSE(t.awaitingActivityRefresh()) << "acquiring must not refresh the clock";
  EXPECT_TRUE(t.release(Need::UsbVirtualDisk, 1000));
  EXPECT_TRUE(t.awaitingActivityRefresh());
  EXPECT_TRUE(t.takeActivityRefresh()) << "the refresh is owed exactly once";
  EXPECT_FALSE(t.takeActivityRefresh()) << "and it must not be owed twice";

  t.acquire(Need::ForegroundNetwork, 2000);
  EXPECT_TRUE(t.release(Need::ForegroundNetwork, 3000));
  EXPECT_FALSE(t.awaitingActivityRefresh()) << "a SOFT release must not touch the inactivity clock";
  EXPECT_FALSE(t.takeActivityRefresh());
}

TEST(PowerNeed, NestedHardReleaseOnlyRefreshesWhenTheLastHolderLetsGo) {
  Tracker t;
  t.acquire(Need::StandbyInhibit, 0);
  t.acquire(Need::StandbyInhibit, 0);
  t.release(Need::StandbyInhibit, 10);
  EXPECT_FALSE(t.awaitingActivityRefresh()) << "a holder still holds it: not a release";
  t.release(Need::StandbyInhibit, 20);
  EXPECT_TRUE(t.awaitingActivityRefresh());
}

TEST(PowerNeed, DoubleReleaseIsCountedAndNeverWraps) {
  Tracker t;
  EXPECT_FALSE(t.release(Need::UsbVirtualDisk, 0)) << "releasing a need nobody holds is a bug";
  EXPECT_EQ(t.count(Need::UsbVirtualDisk), 0);
  EXPECT_EQ(t.doubleReleases(), 1u) << "and it is counted, so a caller bug is visible";

  t.acquire(Need::UsbVirtualDisk, 0);
  EXPECT_TRUE(t.release(Need::UsbVirtualDisk, 1));
  EXPECT_FALSE(t.release(Need::UsbVirtualDisk, 2));
  EXPECT_EQ(t.report(2).doubleReleases, 2u);
  EXPECT_EQ(t.count(Need::UsbVirtualDisk), 0) << "clamped, not wrapped to 255";
}

TEST(PowerNeed, LeakIsAnnouncedOncePerExcursionAndVisible) {
  Tracker t;
  const unsigned long budget = powerneed::leakBudgetMs(Need::StandbyInhibit);
  ASSERT_GT(budget, 0u);

  t.acquire(Need::StandbyInhibit, 0);
  t.poll(budget - 1);
  EXPECT_EQ(t.pendingAnnounceMask(), 0u) << "not a leak before the budget";
  EXPECT_FALSE(t.report(budget - 1).status[static_cast<int>(Need::StandbyInhibit)].leaked);

  t.poll(budget);
  EXPECT_TRUE(t.takeLeakAnnouncement(Need::StandbyInhibit)) << "the leak must be announced at the budget";
  EXPECT_FALSE(t.takeLeakAnnouncement(Need::StandbyInhibit)) << "...exactly once";

  t.poll(budget + 10 * kMinute);
  EXPECT_FALSE(t.takeLeakAnnouncement(Need::StandbyInhibit)) << "no re-announce while the same hold stands";

  const powerneed::Report r = t.report(budget + 10 * kMinute);
  const powerneed::Status& s = r.status[static_cast<int>(Need::StandbyInhibit)];
  EXPECT_TRUE(s.leaked);
  EXPECT_EQ(s.count, 1);
  EXPECT_EQ(s.heldMs, budget + 10 * kMinute);
  EXPECT_EQ(r.leakedNow, 1u);

  // Ending the hold and taking it again is a NEW excursion, so it announces again.
  t.release(Need::StandbyInhibit, budget + 11 * kMinute);
  EXPECT_EQ(t.report(budget + 11 * kMinute).leakedNow, 0u);
  t.acquire(Need::StandbyInhibit, budget + 20 * kMinute);
  t.poll(budget + 20 * kMinute + budget);
  EXPECT_TRUE(t.takeLeakAnnouncement(Need::StandbyInhibit));
  EXPECT_EQ(t.report(budget + 20 * kMinute + budget).leakedEver, 2u);
}

TEST(PowerNeed, SoftNeedAlsoReportsALeakBecauseASessionThatNeverEndsIsTheDefect) {
  Tracker t;
  const unsigned long budget = powerneed::leakBudgetMs(Need::ForegroundNetwork);
  ASSERT_GT(budget, 0u);
  t.acquire(Need::ForegroundNetwork, 0);
  t.poll(budget);
  EXPECT_TRUE(t.takeLeakAnnouncement(Need::ForegroundNetwork));
  EXPECT_FALSE(t.blocksAutoStandby()) << "visible, still inert";
}

TEST(PowerNeed, ChargerHostIsNotAgeMonitoredButIsStillVisible) {
  Tracker t;
  EXPECT_EQ(powerneed::leakBudgetMs(Need::ChargerHost), 0u);
  t.acquire(Need::ChargerHost, 0);
  t.poll(30UL * 24UL * 3600UL * 1000UL);
  EXPECT_FALSE(t.takeLeakAnnouncement(Need::ChargerHost)) << "its owner re-reads the pack every iteration";
  const powerneed::Report r = t.report(5 * kMinute);
  EXPECT_EQ(r.status[static_cast<int>(Need::ChargerHost)].heldMs, 5 * kMinute) << "but its age is reported";
  EXPECT_FALSE(r.status[static_cast<int>(Need::ChargerHost)].leaked)
      << "it is not age-monitored, so its age alone is never called a leak";
  EXPECT_EQ(r.leakedNow, 0u);
}

TEST(PowerNeed, RebaseForgetsTheParkSpanNotTheHold) {
  Tracker t;
  const unsigned long budget = powerneed::leakBudgetMs(Need::StandbyInhibit);
  t.acquire(Need::StandbyInhibit, 0);
  // A standby parks the device for longer than the budget: millis() advances, the hold
  // does not.
  t.rebase(budget * 2);
  EXPECT_EQ(t.count(Need::StandbyInhibit), 1) << "rebase must not release anything";
  EXPECT_EQ(t.report(budget * 2).status[static_cast<int>(Need::StandbyInhibit)].heldMs, 0u);
  t.poll(budget * 2);
  EXPECT_FALSE(t.takeLeakAnnouncement(Need::StandbyInhibit)) << "a park is not a leak";

  // ...and a genuinely stuck hold still ages out from the new base.
  t.poll(budget * 2 + budget);
  EXPECT_TRUE(t.takeLeakAnnouncement(Need::StandbyInhibit));
}

TEST(PowerNeed, DeclarativeSetSyncsAndDoesNotThrash) {
  Tracker t;
  t.set(Need::ChargerHost, true, 0);
  t.set(Need::ChargerHost, true, 1000);
  EXPECT_EQ(t.count(Need::ChargerHost), 1) << "set() is a state, not an acquire per call";
  EXPECT_EQ(t.report(1000).status[static_cast<int>(Need::ChargerHost)].heldMs, 1000u);

  t.set(Need::ChargerHost, false, 2000);
  EXPECT_FALSE(t.held(Need::ChargerHost));
  EXPECT_TRUE(t.awaitingActivityRefresh()) << "ChargerHost is hard: its release refreshes the clock";

  t.set(Need::ChargerHost, false, 3000);
  EXPECT_EQ(t.doubleReleases(), 0u) << "a repeated 'not wanted' is not a double release";
}

// The RAII destructor is a BACKSTOP, not an orderly end of work: a hold that only stopped because
// its scope died must stop blocking a standby without earning a fresh inactivity window.
TEST(PowerNeed, BackstopReleaseEndsTheHoldWithoutRefreshingTheIdleClock) {
  powerneed::sys().reset();
  {
    powerneed::ScopedNeed hold(Need::UsbVirtualDisk);
    hold.acquire(0);
    EXPECT_TRUE(powerneed::blocksAutoStandby());
    // No explicit release: the scope ends and the destructor fires.
  }
  EXPECT_FALSE(powerneed::held(Need::UsbVirtualDisk));
  EXPECT_FALSE(powerneed::blocksAutoStandby());
  EXPECT_FALSE(powerneed::sys().awaitingActivityRefresh())
      << "a leaked hold's backstop must not postpone auto-standby with a fresh idle window";
  EXPECT_FALSE(powerneed::sys().takeActivityRefresh());
  powerneed::sys().reset();
}

TEST(PowerNeed, ScopedHoldIsIdempotentAndReleasesOnScopeExit) {
  powerneed::sys().reset();
  {
    powerneed::ScopedNeed hold(Need::UsbVirtualDisk);
    hold.acquire(0);
    hold.acquire(1);  // idempotent
    EXPECT_EQ(powerneed::sys().count(Need::UsbVirtualDisk), 1);
    EXPECT_TRUE(hold.held());
    EXPECT_TRUE(hold.release(2));
    EXPECT_FALSE(hold.release(3)) << "backstop release after the normal one is free";
  }
  EXPECT_FALSE(powerneed::held(Need::UsbVirtualDisk));
  EXPECT_EQ(powerneed::sys().report(4).doubleReleases, 0u);

  {
    powerneed::ScopedNeed hold(Need::StandbyInhibit);
    hold.acquire(0);
  }
  EXPECT_FALSE(powerneed::held(Need::StandbyInhibit)) << "the destructor is the release";
  EXPECT_EQ(powerneed::sys().report(5).doubleReleases, 0u);
  powerneed::sys().reset();
}

TEST(PowerNeed, UnknownNeedFailsClosedAndDoesNotWriteOutOfBounds) {
  // holdOf() defaults to HARD for a need it does not know: an unclassified hold must
  // block, not silently permit a standby.
  EXPECT_EQ(powerneed::holdOf(static_cast<Need>(200)), Hold::Hard);
}

// ---- item 3.5: the charge-aware idle policy -----------------------------------

TEST(ChargeIdlePolicy, DefaultIsFollowTheTimeoutAndChangesNothing) {
  using chargepolicy::Mode;
  EXPECT_EQ(Mode::FollowTimeout, chargepolicy::modeOf(0));
  // OFF: charging NEVER holds a need, so the user's own timeout keeps its exact
  // meaning and this whole feature is inert.
  EXPECT_FALSE(chargepolicy::holdWhileCharging(0, true));
  EXPECT_FALSE(chargepolicy::holdWhileCharging(0, false));
}

TEST(ChargeIdlePolicy, OnlyChargingOnACableHolds) {
  EXPECT_TRUE(chargepolicy::holdWhileCharging(1, true));
  EXPECT_FALSE(chargepolicy::holdWhileCharging(1, false)) << "on battery: today's behaviour, always";
}

TEST(ChargeIdlePolicy, AnyStoredValueOtherThanOneIsOff) {
  // The backup sanitizer clamps to {0,1}; a settings file written by a newer build
  // must not be able to invent a third behaviour.
  EXPECT_EQ(chargepolicy::modeOf(2), chargepolicy::Mode::FollowTimeout);
  EXPECT_EQ(chargepolicy::modeOf(255), chargepolicy::Mode::FollowTimeout);
}

TEST(ChargeIdlePolicy, ASuppressedAutoSleepIsAHardHoldSoAManualSleepStillSleeps) {
  // The mechanism is the need class, not a branch in the sleep path: nothing in this
  // policy can reach enterDeepSleep().
  Tracker t;
  t.set(Need::ChargerHost, chargepolicy::holdWhileCharging(1, true), 0);
  EXPECT_TRUE(t.blocksAutoStandby());
  t.set(Need::ChargerHost, chargepolicy::holdWhileCharging(1, false), 5000);
  EXPECT_FALSE(t.blocksAutoStandby());
  EXPECT_TRUE(t.awaitingActivityRefresh()) << "cable pulled: the idle clock restarts from here";
}

}  // namespace
