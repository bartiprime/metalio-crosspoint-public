# r163 D3: the grey ADMISSION table (lib/GfxRenderer/ContentRole.h). Header-only, no
# Arduino: the rule that decides whether a pixel may carry an intermediate level is
# checked here, not inferred from the panel.
add_executable(GreyRoleTest
  GreyRoleTest.cpp
)

target_include_directories(GreyRoleTest PRIVATE ${REPO_ROOT}/lib)

target_link_libraries(GreyRoleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GreyRoleTest)

# D3 is an admission rule, never a lane change: this asserts it structurally -- the
# renderer's three writers all ask the one predicate, the tag is scoped (restored on
# every path out), and no tag reaches a refresh-mode decision.
add_executable(GreyRoleWiringTest
  GreyRoleWiringTest.cpp
)

target_compile_definitions(GreyRoleWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(GreyRoleWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GreyRoleWiringTest)
