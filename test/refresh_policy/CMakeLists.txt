add_executable(RefreshPolicyTest
  RefreshPolicyTest.cpp
)

target_include_directories(RefreshPolicyTest PRIVATE
  ${REPO_ROOT}/src/display
)

target_link_libraries(RefreshPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RefreshPolicyTest)