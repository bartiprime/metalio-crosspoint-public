add_executable(Gray16QuantiserTest
  Gray16QuantiserTest.cpp
)

target_include_directories(Gray16QuantiserTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(Gray16QuantiserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Gray16QuantiserTest)
