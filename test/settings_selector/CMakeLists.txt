# SET3 (r164): a settings row with a discrete set of options opens an option
# list. The decision table (SettingsRowKind.h), the dynamic font-family row's
# index <-> stored-state mapping (FontFamilyRow.h) and the SDK's REAL ListNav
# scroll behaviour all run on the host.
add_executable(SettingsSelectorTest
  SettingsSelectorTest.cpp
  # The scroll-position cases drive the SDK's REAL ListNav; its two list-geometry
  # helpers live in FreeInkUI.cpp, linked here exactly as plugin_picker_layout does.
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(SettingsSelectorTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/settings
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_link_libraries(SettingsSelectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsSelectorTest)

# ...and the wiring, as text: the discrete branch opens the list and writes
# nothing, the chosen index is what is persisted, a cancel writes nothing, the
# renderer and the activation path share one decision, and the pop back cannot
# reset the scroll position.
add_executable(SettingsSelectorWiringTest
  SettingsSelectorWiringTest.cpp
)

target_include_directories(SettingsSelectorWiringTest PRIVATE ${REPO_ROOT}/src)

target_compile_definitions(SettingsSelectorWiringTest PRIVATE
  CP_REPO_ROOT="${REPO_ROOT}"
)

target_link_libraries(SettingsSelectorWiringTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsSelectorWiringTest)
