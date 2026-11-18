add_executable(PowerGestureClassifierTest
  PowerGestureClassifierTest.cpp
)

target_include_directories(PowerGestureClassifierTest PRIVATE ${REPO_ROOT}/src)

target_link_libraries(PowerGestureClassifierTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerGestureClassifierTest)
