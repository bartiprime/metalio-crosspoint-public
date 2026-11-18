# r110 WRITEGATE: the oflag predicate, compiled against BOTH SdFat flag encodings.
#
# SdFatConfig.h picks USE_FCNTL_H 1 on ESP32 (system fcntl.h, oflag_t = int) and 0 elsewhere
# (SdFat's own 1/2/4/8/0x10/0x20 set, oflag_t = uint8_t). Two executables, one source file,
# two stubs: the verdicts have to be identical under either, and the suite names must not
# collide in ctest (hence GATE_SUITE).
add_executable(StorageWriteGateTest
  StorageWriteGateTest.cpp
)
target_include_directories(StorageWriteGateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
)
target_compile_definitions(StorageWriteGateTest PRIVATE GATE_SUITE=StorageWriteGate)
target_link_libraries(StorageWriteGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(StorageWriteGateTest)

add_executable(StorageWriteGateFcntlTest
  StorageWriteGateTest.cpp
)
target_include_directories(StorageWriteGateFcntlTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs_fcntl
  ${REPO_ROOT}/lib/hal
)
target_compile_definitions(StorageWriteGateFcntlTest PRIVATE GATE_SUITE=StorageWriteGateFcntl)
target_link_libraries(StorageWriteGateFcntlTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(StorageWriteGateFcntlTest)
