// JPEG1 host test — the decoder-selection table.
//
// What this proves and what it does not:
//   PROVES  which inputs take the hardware path, from their bytes, with the same
//           code the device runs (JpegMarkerScan -> jpegSelectPath).
//   DOES NOT prove the hardware decodes them the same way. That cannot be tested
//           here — there is no codec on the host. It is settled on the device, by
//           the first-use gate in JpegToBmpConverter and by `jpeg compare` on the
//           debug shell.

#include <cstdio>
#include <cstring>
#include <string>

#include "JpegHwPath.h"
#include "JpegHwSelectCases.h"
#include "gtest/gtest.h"

#ifndef CP_REPO_ROOT
#define CP_REPO_ROOT "."
#endif

TEST(JpegHwSelect, SelectionTable) {
  JpegHwCaseCounts counts;
  const auto results = runJpegHwSelectCases(CP_REPO_ROOT, &counts);
  int failed = 0;
  for (const auto& r : results) {
    if (r.skipped) continue;
    if (!r.pass) {
      ++failed;
      ADD_FAILURE() << r.name << ": " << r.detail;
    }
  }
  std::printf("[jpeg1] table: %d cases, %d passed, %d failed, %d skipped\n", counts.total, counts.passed,
              counts.failed, counts.skipped);
  EXPECT_EQ(failed, 0);
  EXPECT_GE(counts.passed, 20);
}

TEST(JpegHwSelect, EveryRefusalIsNamed) {
  // A fallback with no reason is a silent fallback: every non-Eligible enum value
  // must have a human-readable name, and "eligible" must not be reused for a
  // refusal. This is what makes `logtail`/`jpeg status` trustworthy.
  for (int i = 0; i <= static_cast<int>(JpegHwReason::DecodeFailed); i++) {
    const char* n = jpegHwReasonName(static_cast<JpegHwReason>(i));
    ASSERT_NE(n, nullptr);
    ASSERT_GT(strlen(n), 3);
    ASSERT_NE(std::string(n), "?");
  }
}

TEST(JpegHwSelect, GeometryIsPaddedToMcus) {
  // The driver pads the raster to whole MCUs; the luma reader must use the padded
  // stride or every row after the first is shifted.
  JpegProbe p;
  p.headerOk = true;
  p.components = 3;
  p.sampH0 = 2;
  p.sampV0 = 2;
  p.width = 1272;
  p.height = 680;
  EXPECT_EQ(jpegHwMcuWidth(p), 16);
  EXPECT_EQ(jpegHwMcuHeight(p), 16);
  EXPECT_EQ(jpegHwStride(p), 1280u);
  EXPECT_EQ(jpegHwRasterHeight(p), 688u);
  EXPECT_EQ(jpegHwLumaBytesPerPixel(p), 3);
  EXPECT_EQ(jpegHwOutputBytes(p), 1280u * 688u * 3u);

  JpegProbe g;
  g.headerOk = true;
  g.components = 1;
  g.sampH0 = 1;
  g.sampV0 = 1;
  g.width = 600;
  g.height = 800;
  EXPECT_EQ(jpegHwStride(g), 600u);
  EXPECT_EQ(jpegHwRasterHeight(g), 800u);
  EXPECT_EQ(jpegHwLumaBytesPerPixel(g), 1);

  // A 4:2:2 stream has an 8-pixel MCU height, so only the width is padded.
  JpegProbe y422;
  y422.headerOk = true;
  y422.components = 3;
  y422.sampH0 = 2;
  y422.sampV0 = 1;
  y422.width = 900;
  y422.height = 700;
  EXPECT_EQ(jpegHwStride(y422), 912u);
  EXPECT_EQ(jpegHwRasterHeight(y422), 704u);
}
