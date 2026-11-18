add_executable(JpegHwSelectTest
  JpegHwSelectTest.cpp
  JpegHwSelectCases.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegHwPath.cpp
)

target_include_directories(JpegHwSelectTest PRIVATE
  ${REPO_ROOT}/lib/JpegToBmpConverter
)

# The real-JPEG cases read files out of the checkout; if one is absent the case is
# reported as SKIP rather than failing, so this suite works in a clean clone.
target_compile_definitions(JpegHwSelectTest PRIVATE CP_REPO_ROOT="${REPO_ROOT}")

target_link_libraries(JpegHwSelectTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JpegHwSelectTest)
