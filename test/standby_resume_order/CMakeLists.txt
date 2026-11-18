add_executable(StandbyResumeOrderTest
  StandbyResumeOrderTest.cpp
)

target_compile_definitions(StandbyResumeOrderTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(StandbyResumeOrderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StandbyResumeOrderTest
  PROPERTIES LABELS batch0
)
