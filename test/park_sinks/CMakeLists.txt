# r106 B0 — compiles the REAL src/platform/ParkSinks.cpp on the host. It is pure C++
# (the hardware legs fold out without ARDUINO_ARCH_ESP32), so the compiled defaults the
# shipping image carries are exactly the ones this test asserts.
add_executable(ParkSinksTest
  ParkSinksTest.cpp
  ${REPO_ROOT}/src/platform/ParkSinks.cpp
)

target_include_directories(ParkSinksTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ParkSinksTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ParkSinksTest)
