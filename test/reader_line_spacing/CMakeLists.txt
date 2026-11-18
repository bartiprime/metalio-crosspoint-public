add_executable(ReaderLineSpacingTest
  ReaderLineSpacingTest.cpp
)

target_include_directories(ReaderLineSpacingTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReaderLineSpacingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderLineSpacingTest)
