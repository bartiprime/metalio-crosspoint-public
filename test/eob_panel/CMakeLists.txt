# EOB1 (r111) — the end-of-book panel's geometry and repaint decision, on the host.
#
# The header under test is pure (no firmware dependency) and the policy it feeds
# is the SAME file the renderer compiles (src/display/RefreshPolicy.h), so
# "a selection is one DU drive" and "entering is one from-white GC16" are proven
# through the real two-stage producer + policy chain, not against a copy of it.
# The call-site check reads ReaderActivity.cpp (no seam exists under src/).
add_executable(EobPanelTest
  EobPanelRefreshTest.cpp
  EobPanelCallSiteTest.cpp
)

target_include_directories(EobPanelTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
)

target_compile_definitions(EobPanelTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(EobPanelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EobPanelTest)
