#pragma once

// JPEG1 host-test table. Shared by the gtest suite and by the standalone runner
// (which is how it is executed when the full CMake project cannot configure).
//
// Every case goes through the REAL pipeline that runs on the device:
//   bytes -> JpegMarkerScan -> JpegProbe -> jpegSelectPath -> path + reason
// so a case is not "the policy says X for this struct", it is "these bytes select
// X, for this reason".

#include <string>
#include <vector>

struct JpegHwCaseResult {
  std::string name;
  bool pass = false;
  bool skipped = false;
  std::string detail;
};

struct JpegHwCaseCounts {
  int total = 0;
  int passed = 0;
  int failed = 0;
  int skipped = 0;
};

// repoRoot: the crosspoint-reader checkout (for the real-JPEG cases). Empty =
// synthetic cases only.
std::vector<JpegHwCaseResult> runJpegHwSelectCases(const std::string& repoRoot, JpegHwCaseCounts* counts);
