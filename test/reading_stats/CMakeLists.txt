add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ${REPO_ROOT}/src/util/ReadingStats.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
