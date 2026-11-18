add_executable(PolicyPaintTest
  PolicyPaintTest.cpp
)

target_include_directories(PolicyPaintTest PRIVATE
  ${REPO_ROOT}/src/display
)

target_compile_definitions(PolicyPaintTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(PolicyPaintTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PolicyPaintTest
  PROPERTIES LABELS "batch0"
)
