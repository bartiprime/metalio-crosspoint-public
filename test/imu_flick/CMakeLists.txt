add_executable(FlickDetectorTest
  FlickDetectorTest.cpp
)

# The headers under test are lib/hal/FlickDetector.h and
# lib/hal/FlickCalibration.h -- pure C++, no Arduino, no I2C, no clock.
# crosspoint_test_common puts ${REPO_ROOT} and ${REPO_ROOT}/lib on the include
# path, so they are reached as "hal/..." exactly as the firmware includes them.
target_link_libraries(FlickDetectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FlickDetectorTest)
