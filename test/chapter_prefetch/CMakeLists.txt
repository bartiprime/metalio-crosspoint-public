add_executable(ChapterPrefetchTest
  ChapterPrefetchTest.cpp
)

# The last four cases read the reader source: the collision fix ("the prepared section owns its own
# cache path"), the idle placement and the adoption order are properties of the wiring, not of the
# decision functions, so they are gated on the host rather than left to review.
target_compile_definitions(ChapterPrefetchTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_include_directories(ChapterPrefetchTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ChapterPrefetchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterPrefetchTest)
