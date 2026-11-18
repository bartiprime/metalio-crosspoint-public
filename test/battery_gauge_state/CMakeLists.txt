add_executable(BatteryGaugeStateTest
  BatteryGaugeStateTest.cpp
)

target_include_directories(BatteryGaugeStateTest PRIVATE ${REPO_ROOT}/src)

target_link_libraries(BatteryGaugeStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BatteryGaugeStateTest)

# r163 E3: every consumer of the charge state must use the right one of the two answers.
# This is a source-landmark check (there is no seam under src/ for the themes, and this
# batch may not add one) -- same technique as test/standby_resume_order.
add_executable(BatteryGaugeWiringTest
  BatteryGaugeWiringTest.cpp
)

target_compile_definitions(BatteryGaugeWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(BatteryGaugeWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BatteryGaugeWiringTest)
