# CAD2: the cadence pickers' display order (1/5/10/15/30/60/Never, Never last) as a
# pure table, plus the wiring that proves BOTH surfaces -- Settings > Reader and the
# in-book More menu -- read it instead of keeping their own copy.
add_executable(CadenceOrderTest CadenceOrderTest.cpp)

target_include_directories(CadenceOrderTest PRIVATE
  ${REPO_ROOT}/src
  # CadenceOrder.h names StrIds; the enum header is generated into lib/I18n.
  ${REPO_ROOT}/lib/I18n
)

target_link_libraries(CadenceOrderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CadenceOrderTest)

add_executable(CadenceOrderWiringTest CadenceOrderWiringTest.cpp)

target_include_directories(CadenceOrderWiringTest PRIVATE ${REPO_ROOT}/src)

target_compile_definitions(CadenceOrderWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(CadenceOrderWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CadenceOrderWiringTest)
