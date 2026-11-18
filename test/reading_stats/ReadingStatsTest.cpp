#include <gtest/gtest.h>

#include <cstring>

#include "util/ReadingStats.h"

namespace {

using readingstats::Tally;

TEST(ReadingStats, WithholdsRateUnderOneMinuteOfRecordedTime) {
  // 10 pages in 59 s is an arithmetic 610/h, which is not a reading speed. The rule is
  // "no rate under kMinRateSeconds", and the page prints "--" instead.
  const Tally skim{10, 59};
  EXPECT_FALSE(readingstats::haveRate(skim));
  char buf[16];
  readingstats::formatPagesPerHour(skim, buf, sizeof(buf));
  EXPECT_STREQ("--", buf);
}

TEST(ReadingStats, RateIsAvailableAtExactlyOneMinute) {
  const Tally tally{12, readingstats::kMinRateSeconds};
  ASSERT_TRUE(readingstats::haveRate(tally));
  EXPECT_FLOAT_EQ(720.0f, readingstats::pagesPerHour(tally));
  char buf[16];
  readingstats::formatPagesPerHour(tally, buf, sizeof(buf));
  EXPECT_STREQ("720", buf);
}

TEST(ReadingStats, ZeroPagesWithTimeHasNoRate) {
  // Time recorded but nothing turned (a page left open): a denominator without a numerator
  // is not 0 pages/hour, it is "nothing to say".
  const Tally idle{0, 7200};
  EXPECT_FALSE(readingstats::haveRate(idle));
  char buf[16];
  readingstats::formatPagesPerHour(idle, buf, sizeof(buf));
  EXPECT_STREQ("--", buf);
}

TEST(ReadingStats, RateCarriesOneDecimalUnderOneHundred) {
  char buf[16];
  readingstats::formatPagesPerHour(Tally{5, 3600}, buf, sizeof(buf));
  EXPECT_STREQ("5.0", buf);
  readingstats::formatPagesPerHour(Tally{998, 36000}, buf, sizeof(buf));  // 99.8/h
  EXPECT_STREQ("99.8", buf);
}

TEST(ReadingStats, RateDropsTheDecimalAtOneHundred) {
  char buf[16];
  readingstats::formatPagesPerHour(Tally{100, 3600}, buf, sizeof(buf));
  EXPECT_STREQ("100", buf);
}

TEST(ReadingStats, AccumulateSaturatesBothCounters) {
  Tally tally{};
  readingstats::accumulate(tally, 3u, 90u);
  EXPECT_EQ(3u, tally.pages);
  EXPECT_EQ(90u, tally.seconds);

  // Push past the page cap: it saturates, never wraps to a small number.
  const Tally cap{readingstats::kMaxPagesPerBook - 1u, 0u};
  Tally atCap = cap;
  readingstats::accumulate(atCap, 5u, 0u);
  EXPECT_EQ(readingstats::kMaxPagesPerBook, atCap.pages);

  Tally atCapSeconds{0u, readingstats::kMaxSecondsPerBook - 2u};
  readingstats::accumulate(atCapSeconds, 0u, 10u);
  EXPECT_EQ(readingstats::kMaxSecondsPerBook, atCapSeconds.seconds);
}

TEST(ReadingStats, SaturatingAddNeverWraps) {
  EXPECT_EQ(0u, readingstats::saturatingAdd(0u, 0u, 100u));
  EXPECT_EQ(95u, readingstats::saturatingAdd(90u, 5u, 100u));
  EXPECT_EQ(100u, readingstats::saturatingAdd(90u, 20u, 100u));
  EXPECT_EQ(100u, readingstats::saturatingAdd(100u, 0u, 100u));
  // add >= cap short-circuits before the subtraction, so no unsigned underflow.
  EXPECT_EQ(100u, readingstats::saturatingAdd(0u, 100u, 100u));
  EXPECT_EQ(100u, readingstats::saturatingAdd(0u, 4000000000u, 100u));
}

TEST(ReadingStats, AccumulateSumsTalliesAndSaturates) {
  Tally total{5u, 60u};
  readingstats::accumulate(total, Tally{7u, 120u});
  EXPECT_EQ(12u, total.pages);
  EXPECT_EQ(180u, total.seconds);
}

TEST(ReadingStats, DurationFormatsMinutesUnderAnHour) {
  char buf[16];
  readingstats::formatDuration(0, buf, sizeof(buf));
  EXPECT_STREQ("0m", buf);  // under a minute reads as 0m, not "1m": nothing is rounded up
  readingstats::formatDuration(59, buf, sizeof(buf));
  EXPECT_STREQ("0m", buf);
  readingstats::formatDuration(60, buf, sizeof(buf));
  EXPECT_STREQ("1m", buf);
  readingstats::formatDuration(3599, buf, sizeof(buf));
  EXPECT_STREQ("59m", buf);
}

TEST(ReadingStats, DurationFormatsHoursAndMinutes) {
  char buf[16];
  readingstats::formatDuration(3600, buf, sizeof(buf));
  EXPECT_STREQ("1h 00m", buf);
  readingstats::formatDuration(7384, buf, sizeof(buf));  // 2 h 03 m
  EXPECT_STREQ("2h 03m", buf);
  readingstats::formatDuration(100u * 3600u, buf, sizeof(buf));
  EXPECT_STREQ("100h 00m", buf);
}

TEST(ReadingStats, DurationDropsMinutesPastFourDigits) {
  char buf[16];
  // 1000 h is where "999h 59m" stops being the longer string; whole hours keep the row
  // inside a fixed value column.
  readingstats::formatDuration(1000u * 3600u, buf, sizeof(buf));
  EXPECT_STREQ("1000h", buf);
  readingstats::formatDuration(readingstats::kMaxSecondsPerBook, buf, sizeof(buf));
  EXPECT_STREQ("27777h", buf);
}

TEST(ReadingStats, FormattingRespectsAShortBuffer) {
  char buf[4];
  readingstats::formatDuration(3600u * 12u, buf, sizeof(buf));
  EXPECT_LT(std::strlen(buf), sizeof(buf));
  readingstats::formatPagesPerHour(Tally{5, 3600}, buf, sizeof(buf));
  EXPECT_LT(std::strlen(buf), sizeof(buf));
}

TEST(ReadingStats, BoundIsTheStatedDozen) {
  EXPECT_EQ(12, readingstats::kMaxBooks);
  EXPECT_EQ(60u, readingstats::kMinRateSeconds);
  EXPECT_LT(readingstats::kMinDwellMs, 60u * 1000u);
}

}  // namespace
