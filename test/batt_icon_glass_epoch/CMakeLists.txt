add_executable(BattIconGlassEpochTest
  BattIconGlassEpochTest.cpp
)

# The real header, compiled as the renderer compiles it: lib/GfxRenderer is an
# include root, so `#include "GlassEpoch.h"` resolves exactly as it does in
# GfxRenderer.h. No Arduino, no ESP-IDF, no stubs -- the header has no deps by design.
target_include_directories(BattIconGlassEpochTest PRIVATE ${REPO_ROOT}/lib/GfxRenderer)

target_link_libraries(BattIconGlassEpochTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BattIconGlassEpochTest)
