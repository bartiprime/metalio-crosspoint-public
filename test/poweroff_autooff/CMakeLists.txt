add_executable(PowerOffAutoOffTest
  PowerOffAutoOffTest.cpp
)

target_compile_definitions(PowerOffAutoOffTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(PowerOffAutoOffTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PowerOffAutoOffTest)
