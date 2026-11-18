# r163 E4: the charger control loop. Compiles the real header (src/system/ChargerPolicy.h)
# -- there is no seam to fake, so what runs here is what runs on the device, minus Wire.
add_executable(ChargerPolicyTest
  ChargerPolicyTest.cpp
)

target_include_directories(ChargerPolicyTest PRIVATE ${REPO_ROOT}/src)

target_link_libraries(ChargerPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChargerPolicyTest)

# r163 E4: the wiring as text -- main.cpp calls the tick, VREG stays boot-only and 4400, the
# kill switch defaults ON, and the one write site is the one guarded by the allowlist.
add_executable(ChargerWiringTest
  ChargerWiringTest.cpp
)

target_compile_definitions(ChargerWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(ChargerWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChargerWiringTest)
