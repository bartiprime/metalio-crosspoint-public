add_executable(ThumbGreysTest
  ThumbGreysTest.cpp
)

target_include_directories(ThumbGreysTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ThumbGreysTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThumbGreysTest)
