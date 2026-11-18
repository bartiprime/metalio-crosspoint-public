add_executable(PowerOffTextFrameTest
  PowerOffTextFrameTest.cpp
)

target_link_libraries(PowerOffTextFrameTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerOffTextFrameTest)
