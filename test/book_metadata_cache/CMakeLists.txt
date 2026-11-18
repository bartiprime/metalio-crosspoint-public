add_executable(BookMetadataCacheTest
  BookMetadataCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookMetadataCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
)

target_link_libraries(BookMetadataCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# This test replaces global operator new/delete to count resident heap
# (BookMetadataCacheTest.cpp). Under MinGW the C++ runtime is normally a
# DLL, so allocations made inside libstdc++-6.dll go to its own operator new
# while our replacement frees them -- two heaps, and the process dies at
# static-init (0xc0000374 during gtest_discover_tests, before any output).
# Static-linking the C++ runtime puts one operator new in the image, which
# makes the replacement sound again. Host-test-only: never part of a firmware
# build.
if(MINGW)
  target_link_options(BookMetadataCacheTest PRIVATE -static-libstdc++ -static-libgcc)
endif()

gtest_discover_tests(BookMetadataCacheTest)
