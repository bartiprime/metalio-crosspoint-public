add_executable(PrewarmLookaheadTest
  PrewarmLookaheadTest.cpp
)

target_compile_definitions(PrewarmLookaheadTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(PrewarmLookaheadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PrewarmLookaheadTest)
