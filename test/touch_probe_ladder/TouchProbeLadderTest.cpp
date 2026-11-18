#include <gtest/gtest.h>

#include <TouchProbeLadder.h>

using touchladder::Ladder;

// Attempt 1 is the plain probe: nothing before it, and a first-try success ends
// the ladder immediately (this is the shipping happy path — no rail cycle, no
// reset, no second probe).
TEST(TouchProbeLadder, FirstAttemptSuccessEndsLadder) {
  Ladder l;
  EXPECT_TRUE(l.untouched());
  EXPECT_FALSE(l.needsRecovery());
  EXPECT_FALSE(l.ok());

  EXPECT_FALSE(l.recordAttempt(true));
  EXPECT_TRUE(l.ok());
  EXPECT_EQ(1, l.attempts());
  EXPECT_FALSE(l.needsRecovery());
  EXPECT_FALSE(l.exhausted());
}

// One failure requests exactly one recovery, and a second success latches with
// the attempt count visible (that count is what `touch recover` prints).
TEST(TouchProbeLadder, OneFailureThenSuccessLatches) {
  Ladder l;
  EXPECT_TRUE(l.recordAttempt(false));
  EXPECT_EQ(1, l.attempts());
  EXPECT_TRUE(l.needsRecovery());
  EXPECT_FALSE(l.ok());
  EXPECT_FALSE(l.exhausted());

  EXPECT_FALSE(l.recordAttempt(true));
  EXPECT_TRUE(l.ok());
  EXPECT_EQ(2, l.attempts());
  EXPECT_FALSE(l.needsRecovery());
  EXPECT_FALSE(l.exhausted());
}

// Three failures exhaust the ladder and it STAYS exhausted: no fourth probe,
// and no recovery work for a probe that will not happen.
TEST(TouchProbeLadder, ThreeFailuresExhaust) {
  Ladder l;
  EXPECT_TRUE(l.recordAttempt(false));
  EXPECT_TRUE(l.recordAttempt(false));
  EXPECT_FALSE(l.recordAttempt(false));

  EXPECT_EQ(3, l.attempts());
  EXPECT_EQ(Ladder::kMaxAttempts, l.attempts());
  EXPECT_FALSE(l.ok());
  EXPECT_TRUE(l.exhausted());
  EXPECT_FALSE(l.needsRecovery());
}

// A success at the last legal attempt still latches — "exhausted" must never be
// reported for a ladder that ended in success.
TEST(TouchProbeLadder, SuccessOnThirdAttemptIsNotExhausted) {
  Ladder l;
  EXPECT_TRUE(l.recordAttempt(false));
  EXPECT_TRUE(l.recordAttempt(false));
  EXPECT_FALSE(l.recordAttempt(true));

  EXPECT_EQ(3, l.attempts());
  EXPECT_TRUE(l.ok());
  EXPECT_FALSE(l.exhausted());
  EXPECT_FALSE(l.needsRecovery());
}

// The recovery decision is the only thing that can put the rail down, so it must
// be false on success at every point in the ladder.
TEST(TouchProbeLadder, RecoveryNeverRequestedAfterSuccess) {
  for (int successAt = 1; successAt <= Ladder::kMaxAttempts; ++successAt) {
    Ladder l;
    for (int attempt = 1; attempt <= successAt; ++attempt) {
      l.recordAttempt(attempt == successAt);
    }
    EXPECT_TRUE(l.ok());
    EXPECT_EQ(successAt, l.attempts());
    EXPECT_FALSE(l.needsRecovery());
    EXPECT_FALSE(l.exhausted());
  }
}
