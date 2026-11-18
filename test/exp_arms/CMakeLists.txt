# EXP1 (r101.21) — the arm-defaults gate. Compiles src/platform/ExpArms.cpp on the
# host: it is pure C++ (no Arduino, no IDF), so the compiled defaults a shipping
# image would carry are the ones this test asserts.
add_executable(ExpArmsTest
  ExpArmsTest.cpp
  ${REPO_ROOT}/src/platform/ExpArms.cpp
)

target_include_directories(ExpArmsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ExpArmsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ExpArmsTest)
