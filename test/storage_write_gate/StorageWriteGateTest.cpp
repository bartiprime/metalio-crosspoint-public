// r110 WRITEGATE -- host tests for halstorage::isWriteOflag().
//
// WHY THIS EXISTS: r109 gated the named write helpers but left HalStorage::open() ungated, and
// O_CREAT writes a directory entry AT OPEN (FatFileLFN.cpp create()) while O_TRUNC zeroes the
// file at open. The predicate is therefore the thing that decides whether a markerless boot can
// touch the card through open(), and it must be right under BOTH flag encodings SdFat selects
// between (USE_FCNTL_H 1 on ESP32, 0 elsewhere) -- naming the macros is not the same as knowing
// their values, so this file is compiled twice, once per stub.
//
// GATE_SUITE is defined per target in CMakeLists.txt so the two builds cannot collide in ctest.

#include <gtest/gtest.h>

#include "StorageWriteGate.h"

namespace {

using halstorage::isWriteOflag;

TEST(GATE_SUITE, ReadOnlyOpensAreNotWrites) {
  EXPECT_FALSE(isWriteOflag(O_RDONLY));
  // O_AT_END only positions the handle at EOF; a read-only handle at EOF is still read-only.
  EXPECT_FALSE(isWriteOflag(O_RDONLY | O_AT_END));
  EXPECT_FALSE(isWriteOflag(O_RDONLY | O_EXCL));
}

TEST(GATE_SUITE, WriteAccessModesAreWrites) {
  EXPECT_TRUE(isWriteOflag(O_WRONLY));
  EXPECT_TRUE(isWriteOflag(O_WRITE));
  EXPECT_TRUE(isWriteOflag(O_RDWR));
  EXPECT_TRUE(isWriteOflag(O_WRONLY | O_AT_END));
  EXPECT_TRUE(isWriteOflag(O_RDWR | O_AT_END));
}

TEST(GATE_SUITE, CreateTruncAndAppendAreWritesWithoutAWriteAccessMode) {
  // These are the shapes that write through a read-only ACCESS MODE, which is exactly why the
  // access-mode test alone is not enough.
  EXPECT_TRUE(isWriteOflag(O_RDONLY | O_CREAT));
  EXPECT_TRUE(isWriteOflag(O_RDONLY | O_TRUNC));
  EXPECT_TRUE(isWriteOflag(O_RDONLY | O_APPEND));
}

TEST(GATE_SUITE, TheReachableCallSiteFlagsAreRefused) {
  // LibraryPartial.cpp:171 -- O_RDWR|O_CREAT|O_AT_END (the destructive one: create at open).
  EXPECT_TRUE(isWriteOflag(O_RDWR | O_CREAT | O_AT_END));
  // HalSystem.cpp:110 -- /crash_report.txt, O_TRUNC at open.
  EXPECT_TRUE(isWriteOflag(O_WRONLY | O_CREAT | O_TRUNC));
  // Logging.cpp logSdFlush -- O_WRONLY|O_CREAT|O_APPEND, once per 5 s on a markerless boot.
  EXPECT_TRUE(isWriteOflag(O_WRONLY | O_CREAT | O_APPEND));
}

}  // namespace
