add_executable(BackupProfileTest
  BackupProfileTest.cpp
  ${REPO_ROOT}/lib/Backup/BackupProfile.cpp
  ${REPO_ROOT}/lib/Backup/BackupSha256.cpp
)

target_include_directories(BackupProfileTest PRIVATE
  ${REPO_ROOT}/lib/Backup
  ${REPO_ROOT}/idf/components/extern/ArduinoJson/src
)

target_compile_options(BackupProfileTest PRIVATE -fno-exceptions)
target_link_libraries(BackupProfileTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BackupProfileTest
  PROPERTIES LABELS batch0
)
