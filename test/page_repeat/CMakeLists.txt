add_executable(PageRepeatTest
  PageRepeatTest.cpp
)

target_include_directories(PageRepeatTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PageRepeatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageRepeatTest)
