add_executable(ProgressSavesTest
  ProgressSavesTest.cpp
)

# r163 C4: the coalescing rules (merge to latest, peek for a reopen, a completion that cannot clear
# a newer snapshot) over the real ProgressSaves, plus the destructor rule over the reader source.
target_compile_definitions(ProgressSavesTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_include_directories(ProgressSavesTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ProgressSavesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressSavesTest)
