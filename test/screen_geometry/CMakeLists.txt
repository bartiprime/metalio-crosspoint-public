add_executable(ScreenGeometryTest
  ScreenGeometryTest.cpp
)

target_include_directories(ScreenGeometryTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(ScreenGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ScreenGeometryTest)
