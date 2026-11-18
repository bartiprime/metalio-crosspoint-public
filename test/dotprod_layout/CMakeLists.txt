# SIMD1: the s8 dot-product descriptor and arp4-selection logic, checked on the
# HOST. The vendored ANSI kernel is compiled into this target so the reference
# walk in src/debug/DotprodBenchMath.h is validated against esp-dsp itself, not
# against a second copy of the same assumption. A wrong image2d_t fails here,
# before it can produce a ratio on the glass.
set(SIMD1_DSP ${REPO_ROOT}/idf/managed_components/espressif__esp-dsp)

add_executable(dotprod_layout_test
  test_dotprod_layout.cpp
  ${SIMD1_DSP}/modules/dotprod/fixed/dspi_dotprod_s8_ansi.c
)

target_include_directories(dotprod_layout_test PRIVATE
  ${REPO_ROOT}/src
  ${SIMD1_DSP}/modules/common/include
  ${SIMD1_DSP}/modules/common/include_sim
  ${SIMD1_DSP}/modules/dotprod/include
)

target_link_libraries(dotprod_layout_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(dotprod_layout_test)

# SIMD1: the bench's own decision path (shape table -> values -> report), on the
# host. DotprodBench.cpp is compiled here with CONFIG_DSP_ANSI=1 so
# `dspi_dotprod_s8` resolves to its ANSI twin: that is the "no vector kernel in
# this build" case, and the report must say so instead of printing a 1.00x ratio.
add_executable(dotprod_bench_report_test
  test_dotprod_bench_report.cpp
  ${REPO_ROOT}/src/debug/DotprodBench.cpp
  ${SIMD1_DSP}/modules/dotprod/fixed/dspi_dotprod_s8_ansi.c
)

target_include_directories(dotprod_bench_report_test PRIVATE
  ${REPO_ROOT}/src
  ${SIMD1_DSP}/modules/common/include
  ${SIMD1_DSP}/modules/common/include_sim
  ${SIMD1_DSP}/modules/dotprod/include
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_compile_definitions(dotprod_bench_report_test PRIVATE CONFIG_DSP_ANSI=1)

target_link_libraries(dotprod_bench_report_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(dotprod_bench_report_test)
