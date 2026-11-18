add_executable(ReaderHandoffTest
  ReaderHandoffTest.cpp
)

# r163 C3: the last two cases read the reader sources, so the rules that cannot be expressed in
# the type system (guard checked before use, no raw Try acquisition) are gated on the host.
target_compile_definitions(ReaderHandoffTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_include_directories(ReaderHandoffTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReaderHandoffTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderHandoffTest)
