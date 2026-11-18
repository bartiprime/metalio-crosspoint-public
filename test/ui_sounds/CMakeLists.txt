# AUDIO1 — the UI-sound policy, compiled and driven on the host.
#
# The test links ONLY src/audio/UiSoundPolicy.h, which is the same header the
# firmware's UiSounds.cpp includes. Nothing here is a model of the policy: it is
# the policy.
add_executable(UiSoundsTest
  UiSoundsTest.cpp
)

target_include_directories(UiSoundsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(UiSoundsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UiSoundsTest
  PROPERTIES LABELS batch0
)
