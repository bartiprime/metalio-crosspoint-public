add_executable(AudGateTest
  AudGateTest.cpp
)

target_compile_definitions(AudGateTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(AudGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AudGateTest
  PROPERTIES LABELS batch0
)
