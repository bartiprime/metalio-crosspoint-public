# r163 E6 / 3.5: the power-need algebra. Header-only on purpose (src/system/PowerNeed.h
# owns no I/O), so what runs here is what runs on the device minus millis().
add_executable(PowerNeedTest
  PowerNeedTest.cpp
)

target_include_directories(PowerNeedTest PRIVATE ${REPO_ROOT}/src)

target_link_libraries(PowerNeedTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerNeedTest)

# r163 E6 / 3.5: the WIRING as text -- the sleep gate asks the need table, the park
# sequence contains no need code at all, the tick refreshes the clock on a hard
# release, and the setting is persisted, surfaced and carryable through a restore.
add_executable(PowerNeedWiringTest
  PowerNeedWiringTest.cpp
)

target_include_directories(PowerNeedWiringTest PRIVATE ${REPO_ROOT}/src)

target_compile_definitions(PowerNeedWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(PowerNeedWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerNeedWiringTest)
