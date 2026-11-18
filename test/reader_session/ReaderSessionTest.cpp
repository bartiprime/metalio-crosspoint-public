#include <gtest/gtest.h>

#include "ReaderSession.h"

namespace {

TEST(ReaderSession, UsesOnlyBoundedScalarState) { EXPECT_LE(sizeof(ReaderSession), 48u); }

void anchor(ReaderSession& session, uint32_t ms, int64_t epoch, int progress) {
  session.onRenderComplete(ms, epoch, progress);
}

void turnAndRender(ReaderSession& session, bool forward, bool succeeded, uint32_t ms, int64_t epoch, int progress) {
  session.noteTurn(forward, succeeded);
  session.onRenderComplete(ms, epoch, progress);
}

TEST(ReaderSession, IgnoresDwellsUnderTwoSeconds) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 2999, 102, 1100);

  EXPECT_EQ(session.durationSeconds(), 0u);
  EXPECT_FALSE(session.isEmitWorthy());
}

TEST(ReaderSession, AccumulatesSuccessfulForwardDwellsAndCompactsEndTime) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 6000, 105, 1100);
  turnAndRender(session, true, true, 13000, 112, 1200);

  EXPECT_EQ(session.startTime(), 100);
  EXPECT_EQ(session.durationSeconds(), 12u);
  EXPECT_EQ(session.endTime(), 112);
  EXPECT_EQ(session.endTime(), session.startTime() + session.durationSeconds());
  EXPECT_EQ(session.startProgressBp(), 1000);
  EXPECT_EQ(session.endProgressBp(), 1200);
  EXPECT_TRUE(session.isEmitWorthy());
}

TEST(ReaderSession, ExcludesBackwardSkipAndFailedTurns) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, false, true, 11000, 110, 900);
  turnAndRender(session, true, false, 21000, 120, 1100);
  session.noteTurn(false, false);  // owner uses this signal for skips
  session.onRenderComplete(31000, 130, 2000);

  EXPECT_EQ(session.durationSeconds(), 0u);
}

TEST(ReaderSession, CapsOneDwellAtThirtyMinutes) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  turnAndRender(session, true, true, 3601000, 3700, 2000);

  EXPECT_EQ(session.durationSeconds(), 1800u);
  EXPECT_EQ(session.endTime(), 1900);
}

TEST(ReaderSession, KeepsFirstEpochAndProgressAcrossContributions) {
  ReaderSession session;
  anchor(session, 1000, 1000, 3000);
  turnAndRender(session, true, true, 5000, 1004, 3100);
  turnAndRender(session, true, true, 11000, 1010, 3300);

  EXPECT_EQ(session.startTime(), 1000);
  EXPECT_EQ(session.startProgressBp(), 3000);
  EXPECT_EQ(session.endProgressBp(), 3300);
  EXPECT_EQ(session.durationSeconds(), 10u);
}

TEST(ReaderSession, ClampsProgressRangeAndMonotonicEnd) {
  ReaderSession session;
  anchor(session, 1000, 100, -50);
  turnAndRender(session, true, true, 7000, 106, 12000);
  turnAndRender(session, true, true, 13000, 112, 5000);

  EXPECT_EQ(session.startProgressBp(), 0);
  EXPECT_EQ(session.endProgressBp(), 10000);
}

TEST(ReaderSession, RequiresTenSecondsAndTrustedStart) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 11000, 10, 1100);
  EXPECT_EQ(session.durationSeconds(), 0u);

  turnAndRender(session, true, true, 20000, 19, 1200);
  EXPECT_EQ(session.durationSeconds(), 9u);
  EXPECT_FALSE(session.isEmitWorthy());

  turnAndRender(session, true, true, 22000, 21, 1300);
  EXPECT_EQ(session.durationSeconds(), 11u);
  EXPECT_TRUE(session.isEmitWorthy());
  EXPECT_GT(session.endTime(), session.startTime());
}

TEST(ReaderSession, ResetClearsSummaryAnchorAndPendingTurn) {
  ReaderSession session;
  anchor(session, 1000, 100, 1000);
  session.noteTurn(true, true);
  session.reset();
  session.onRenderComplete(12000, 111, 2000);

  EXPECT_EQ(session.startTime(), 0);
  EXPECT_EQ(session.durationSeconds(), 0u);
  EXPECT_EQ(session.startProgressBp(), 0);
  EXPECT_EQ(session.endProgressBp(), 0);
  EXPECT_FALSE(session.isEmitWorthy());
}

TEST(ReaderSession, MonotonicElapsedHandlesMillisWrap) {
  ReaderSession session;
  anchor(session, 0xfffffc18u, 100, 1000);
  turnAndRender(session, true, true, 4000u, 105, 1100);

  EXPECT_EQ(session.durationSeconds(), 5u);
}

// ---- r163 3.3: the statistics side ---------------------------------------
// Same anchor pair, no epoch needed: these are the numbers ReadingStatsStore is fed.

TEST(ReaderSession, CountsForwardTurnAndItsDwell) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 61000, 0, 1100);

  EXPECT_EQ(session.pagesTurned(), 1u);
  EXPECT_EQ(session.activeMillis(), 60000u);
  EXPECT_TRUE(session.hasActivity());
  // No trusted epoch, so the plugin-event summary stays empty: two independent readers.
  EXPECT_EQ(session.durationSeconds(), 0u);
  EXPECT_FALSE(session.isEmitWorthy());
}

TEST(ReaderSession, CountsPageButNotTimeForASkim) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 1999, 0, 1100);  // 999 ms: flipped, not read

  EXPECT_EQ(session.pagesTurned(), 1u);
  EXPECT_EQ(session.activeMillis(), 0u);
}

TEST(ReaderSession, IgnoresBackwardAndFailedTurnsForStatistics) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, false, true, 11000, 0, 900);
  turnAndRender(session, true, false, 21000, 0, 1100);

  EXPECT_EQ(session.pagesTurned(), 0u);
  EXPECT_EQ(session.activeMillis(), 0u);
  EXPECT_FALSE(session.hasActivity());
}

TEST(ReaderSession, CapsOneDwellAtThirtyMinutesOnTheStatisticsSide) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 3601000, 0, 1100);

  EXPECT_EQ(session.pagesTurned(), 1u);
  EXPECT_EQ(session.activeMillis(), 1800000u);
}

TEST(ReaderSession, StatisticsAccumulateAcrossTurns) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 31000, 0, 1100);
  turnAndRender(session, true, true, 61000, 0, 1200);
  turnAndRender(session, true, true, 66000, 0, 1300);  // 5 s

  EXPECT_EQ(session.pagesTurned(), 3u);
  EXPECT_EQ(session.activeMillis(), 65000u);
}

TEST(ReaderSession, TakeStatisticsHandsTheWindowOverAndClearsIt) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 31000, 0, 1100);
  turnAndRender(session, true, true, 61000, 0, 1200);

  uint32_t pages = 0;
  uint32_t ms = 0;
  session.takeStatistics(pages, ms);
  EXPECT_EQ(pages, 2u);
  EXPECT_EQ(ms, 60000u);
  // The next window starts empty; the plugin-event summary is untouched.
  EXPECT_FALSE(session.hasActivity());
  session.takeStatistics(pages, ms);
  EXPECT_EQ(pages, 0u);
  EXPECT_EQ(ms, 0u);
}

TEST(ReaderSession, ResetClearsTheStatisticsCounters) {
  ReaderSession session;
  anchor(session, 1000, 0, 1000);
  turnAndRender(session, true, true, 61000, 0, 1100);
  session.reset();

  EXPECT_EQ(session.pagesTurned(), 0u);
  EXPECT_EQ(session.activeMillis(), 0u);
  EXPECT_FALSE(session.hasActivity());
}

}  // namespace
