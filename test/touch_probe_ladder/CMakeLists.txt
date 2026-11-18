add_executable(TouchProbeLadderTest
  TouchProbeLadderTest.cpp
)

target_include_directories(TouchProbeLadderTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(TouchProbeLadderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchProbeLadderTest)
