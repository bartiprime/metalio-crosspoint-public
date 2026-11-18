add_executable(ReaderJobQueueTest
  ReaderJobQueueTest.cpp
)

# r163 C2: the bounded queue and the domain's submit/idle-exit decisions, host-side. The FreeRTOS
# task itself is not host-testable; what it decides is, and that is what this pins.
target_compile_definitions(ReaderJobQueueTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_include_directories(ReaderJobQueueTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReaderJobQueueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderJobQueueTest)
