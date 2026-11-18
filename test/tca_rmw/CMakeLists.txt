# r106 B1: the TCA port read-modify-write decision half. The header is the code the
# board layer includes (it is C-compatible because the board is a C translation unit),
# so this asserts the exact bytes a `trim cam cut` will write on glass.
add_executable(TcaRmwTest
  TcaRmwTest.cpp
)

target_include_directories(TcaRmwTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TcaRmwTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TcaRmwTest)
