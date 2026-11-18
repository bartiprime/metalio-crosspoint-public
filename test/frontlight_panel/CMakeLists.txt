# FLP1 (r101.21b) — the frontlight panel's repaint decision, on the host.
#
# The header under test is pure (no firmware dependency) and the policy it feeds
# is the SAME file the renderer compiles (src/display/RefreshPolicy.h), so
# "adjust resolves to FAST" and "exit resolves to exactly one from-white FULL"
# are proven through the real two-stage producer + policy chain, not against a
# copy of it.
add_executable(FrontlightPanelRefreshTest
  FrontlightPanelRefreshTest.cpp
)

target_include_directories(FrontlightPanelRefreshTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
)

target_link_libraries(FrontlightPanelRefreshTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FrontlightPanelRefreshTest)
