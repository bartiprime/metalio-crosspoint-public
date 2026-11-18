// SIMD1 — the bench's own decision path, on the HOST.
//
// Two things are checked here and neither needs the glass:
//   1. run() wires the shape table, the seed, the descriptors and the reference
//      together correctly: every shape must be eligible, fit the int8 output,
//      and produce a value that all three implementations agree on.
//   2. formatReport() must NOT print a ratio when the vector kernel is not what
//      `dspi_dotprod_s8` resolves to. This target is compiled with
//      CONFIG_DSP_ANSI=1, so `dspi_dotprod_s8` IS the ANSI twin and
//      `fastPath` is false -- exactly the "no SIMD in this build" case, which
//      must report that instead of a fake 1.00x.
//
// The r101.21 failure (`ratio=33.84x`, kernel=-74 vs ansi=3) could not be
// reproduced here: the divergence is in the arp4 kernel, which this host cannot
// execute. What this test does prove is that the numbers a ratio would be built
// on are right for the ANSI path, and that the refusal text is what the shell
// will actually print when agreement fails.

#include "debug/DotprodBench.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

// The bench measures with the IDF timer; on the host a monotonic counter is
// enough, because this test only asserts the REPORT shape, not timings.
extern "C" int64_t esp_timer_get_time(void) {
  static int64_t now = 0;
  return now += 100;
}

namespace {

std::string report(const dotprodb::Result& r) {
  char buf[1600];
  dotprodb::formatReport(r, buf, sizeof(buf));
  return std::string(buf);
}

}  // namespace

TEST(DotprodBenchReport, EveryShapeIsEligibleAndAgreesWithTheReference) {
  const dotprodb::Result r = dotprodb::run(1);
  ASSERT_TRUE(r.built);
  EXPECT_FALSE(r.fastPath) << "this target is built with CONFIG_DSP_ANSI";
  EXPECT_EQ(r.shapeCount, 5);
  EXPECT_EQ(r.agreeCount, r.shapeCount);
  for (int i = 0; i < r.shapeCount; ++i) {
    const auto& s = r.shapes[i];
    EXPECT_TRUE(s.fits) << "shape " << s.countX << "x" << s.countY;
    EXPECT_TRUE(s.agree) << "shape " << s.countX << "x" << s.countY;
    EXPECT_EQ(s.kernelValue, s.refValue) << s.countX << "x" << s.countY;
    EXPECT_EQ(s.ansiValue, s.refValue) << s.countX << "x" << s.countY;
    EXPECT_GT(s.ansiUs, 0u);
    EXPECT_GT(s.kernelUs, 0u);
  }
  EXPECT_TRUE(r.ok);  // values agree; only fastPath is missing on this host
  EXPECT_EQ(r.ratioX100, 0u) << "no ratio may be computed without the vector kernel";
}

TEST(DotprodBenchReport, ReportNamesEveryShapeAndWithholdsTheRatio) {
  const dotprodb::Result r = dotprodb::run(1);
  const std::string text = report(r);

  EXPECT_NE(text.find("64x32"), std::string::npos) << text;
  EXPECT_NE(text.find("128x16"), std::string::npos) << text;
  EXPECT_NE(text.find("16x64"), std::string::npos) << text;
  EXPECT_EQ(text.find("ratio="), std::string::npos)
      << "a ratio must not be printed when dspi_dotprod_s8 is the ANSI twin:\n"
      << text;
  EXPECT_NE(text.find("ANSI twin"), std::string::npos) << text;
}

TEST(DotprodBenchReport, ReportFitsTheShellBuffer) {
  // `exp` is handed 768 bytes by UsbShell (char out[768]); the verdict line is
  // last, so an overflow would silently cut the very sentence that withholds the
  // ratio. Keep the whole report comfortably inside that budget.
  const dotprodb::Result r = dotprodb::run(1);
  char buf[1600];
  dotprodb::formatReport(r, buf, sizeof(buf));
  EXPECT_LT(std::strlen(buf), 760u);
}

TEST(DotprodBenchReport, ValuesDifferLineNamesShapeAndBothValues) {
  // A synthetic disagreement, exactly the shape of the r101.21 output: the
  // reporter must name the shape and both values, and must not print a ratio.
  dotprodb::Result r;
  r.built = true;
  r.fastPath = true;
  r.iterations = 200;
  r.shapeCount = 2;
  r.agreeCount = 1;
  r.shapes[0].countX = 64;
  r.shapes[0].countY = 32;
  r.shapes[0].refValue = 3;
  r.shapes[0].kernelValue = 3;
  r.shapes[0].ansiValue = 3;
  r.shapes[0].fits = true;
  r.shapes[0].agree = true;
  r.shapes[1].countX = 128;
  r.shapes[1].countY = 16;
  r.shapes[1].refValue = 3;
  r.shapes[1].kernelValue = -74;
  r.shapes[1].ansiValue = 3;
  r.shapes[1].fits = true;
  r.shapes[1].agree = false;
  r.ok = false;
  r.ratioX100 = 3384;

  const std::string text = report(r);
  EXPECT_NE(text.find("VALUES DIFFER"), std::string::npos) << text;
  EXPECT_NE(text.find("128x16"), std::string::npos) << text;
  EXPECT_NE(text.find("-74"), std::string::npos)
      << "the disagreeing value must be printed:\n"
      << text;
  EXPECT_EQ(text.find("ratio="), std::string::npos)
      << "the synthetic run's ratioX100 must not reach the output:\n"
      << text;
}
