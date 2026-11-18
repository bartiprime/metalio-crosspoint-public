# cp_lp_core — the ULP-LP-core binary and the C shim around it.
#
# WHY THIS IS A COMPONENT AND NOT PART OF crosspoint_app
#   ulp_embed_binary() (components/ulp/project_include.cmake:142) builds the ULP
#   program as a NESTED cmake project with its own toolchain
#   (components/ulp/cmake/toolchain-lp-core-riscv.cmake -> riscv32-esp-elf-gcc)
#   and attaches the resulting .bin/.ld/.h to ONE component's library. Keeping
#   that in its own component means:
#     * the application tree never sees a ULP header or the riscv toolchain;
#     * a target without a ULP co-processor still builds this component (it
#       compiles to the ESP_ERR_NOT_SUPPORTED stubs) instead of failing.
#
# WHEN THE COMPONENT IS PART OF THE BUILD AT ALL (r99)
#   Only when CP_BENCH or CP_LP_STANDBY is ON (both options live in idf/CMakeLists.txt
#   so a COMPONENT can see them; a compile definition on crosspoint_app is set too
#   late and does not reach CMake). With BOTH off this file registers the component
#   with ZERO sources and returns: no ULP program is built, no nested riscv build
#   runs, no cp_lp_* object exists, and nothing here can change the shipping image.
#
#   This is what makes the shipping build inert rather than merely unused: the
#   sources used to be registered unconditionally, so a shim that includes the
#   mapgen-generated header failed to compile in a configuration where the ULP
#   build had not produced that header (`ulp_cp_lp_sentry_hb undeclared`).
#   Registration is now the switch, not a #if inside the sources.
#
# WHEN THE ULP IS *NOT* BUILT
#   CP_BENCH=ON with CONFIG_ULP_COPROC_ENABLED unset still registers the shims,
#   which compile to the ESP_ERR_NOT_SUPPORTED stubs with CP_LP_CORE_BUILT=0, so a
#   bench image on such a configuration is honest instead of broken.

set(cp_lp_enabled 0)
if(CP_BENCH OR CP_LP_STANDBY)
    set(cp_lp_enabled 1)
endif()

if(NOT cp_lp_enabled)
    # Header-only: src/platform/LpCoreService.cpp includes <cp_lp_core.h>
    # unconditionally, so the include dir must exist, but with no sources there is
    # nothing in the archive and nothing to link.
    # NOTE: do NOT call target_compile_definitions(${COMPONENT_LIB} ...) here — a
    # component registered with ZERO sources has no ${COMPONENT_LIB} target, and the
    # call fails at CONFIGURE time, taking the whole shipping build with it.
    # cp_lp_core.h already defaults CP_LP_CORE_BUILT to 0 when it is not defined.
    idf_component_register(SRCS "" INCLUDE_DIRS "include")
    message(STATUS "cp_lp_core: off (CP_BENCH=OFF, CP_LP_STANDBY=OFF) — no sources, no ULP build")
    return()
endif()

if(CONFIG_ULP_COPROC_ENABLED AND CONFIG_ULP_COPROC_TYPE_LP_CORE)
    set(cp_lp_srcs "cp_lp_core.c" "cp_lp_bench.c" "cp_lp_sentry.c")
    set(cp_lp_ulp_built 1)
else()
    set(cp_lp_srcs "cp_lp_core.c" "cp_lp_bench.c" "cp_lp_sentry.c")
    set(cp_lp_ulp_built 0)
endif()

idf_component_register(
    SRCS ${cp_lp_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ulp esp_driver_gpio
)
# NO WHOLE_ARCHIVE, deliberately (changed in r87). Force-including this archive
# meant the shim object — and through its references the whole ulp_lp_core_*
# driver and the embedded blob — landed in EVERY image, including one built with
# CP_LP_CORE=0. Archive resolution is enough: when LpCoreService.cpp calls
# cp_lp_core_start(), cp_lp_core.o is pulled in, and its reference to
# _binary_ulp_cp_lp_core_bin_start pulls the blob object from the same archive.
# With the switch off nothing in the image refers to any of it, so the linker
# keeps none of it — which is what makes the shipping default provably free of
# accelerator code.

target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_LP_CORE_BUILT=${cp_lp_ulp_built})

if(cp_lp_ulp_built)
    # ---- WHY THE INCLUDE DIRECTORIES BELOW ARE ADDED BY HAND ----------------
    # The nested ULP project receives EXACTLY this component's
    # INTERFACE_INCLUDE_DIRECTORIES and nothing else
    # (components/ulp/project_include.cmake:91 -> cmake/IDFULPProject.cmake:75).
    # IDF's own LP-core library sources, however, include headers from components
    # that list never contained: lp_core_i2c.c wants hal/i2c_types.h,
    # ulp_lp_core_lp_timer_shared.c wants hal/rtc_timer_ll.h, and the ULP build's
    # esp_driver_uart/src/uart_wakeup.c wants driver/uart_wakeup.h. On a target
    # where SOC_LP_I2C_SUPPORTED / SOC_ULP_LP_UART_SUPPORTED / RTC_TIMER_V3 are
    # all 1 — esp32s31 — the LP build therefore fails with "No such file or
    # directory" before a single line of our program is compiled.
    #
    # This is the "declared but not exercised" state docs/LP_CORE_CAPABILITY.md §6.3
    # warned about, and it is a one-line fix from the OUTSIDE: publish those
    # components' include dirs on this target, and the nested project inherits
    # them. Guarded on TARGET so a configuration without one of them still
    # configures instead of erroring.
    foreach(_cp_lp_dep ulp esp_driver_gpio esp_driver_uart esp_hal_i2c esp_hal_rtc_timer
                        esp_hal_touch_sens esp_hal_gpspi esp_hal_clock esp_adc esp_adc_oneshot)
        if(TARGET __idf_${_cp_lp_dep})
            get_target_property(_cp_lp_dep_incs __idf_${_cp_lp_dep} INTERFACE_INCLUDE_DIRECTORIES)
            if(_cp_lp_dep_incs)
                # PUBLIC, not INTERFACE: this component's OWN sources need these
                # headers too (cp_lp_core.c includes <ulp_lp_core.h>,
                # cp_lp_sentry.c includes <driver/rtc_io.h>), and the nested ULP
                # project inherits whatever is on the INTERFACE half of PUBLIC.
                target_include_directories(${COMPONENT_LIB} PUBLIC ${_cp_lp_dep_incs})
            endif()
        endif()
    endforeach()

    # The ULP support additions, exactly as IDF's own LP-core examples do them
    # (examples/system/ulp/lp_core/gpio/main/CMakeLists.txt):
    #   1. a unique app name -> ulp_cp_lp_core.{bin,ld,h} in the build dir;
    #   2. the ULP C sources, in their own directory (not in SRCS above);
    #   3. which sources include the generated header — they get an OBJECT_DEPENDS
    #      on the artifacts, so a changed ULP program rebuilds the shim too.
    set(ulp_app_name "ulp_cp_lp_core")
    set(ulp_sources "ulp/main.c")
    set(ulp_exp_dep_srcs "cp_lp_core.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
    message(STATUS "cp_lp_core: building ULP-LP-core program ${ulp_app_name}")

    # r96: the SECOND program, the LP bench (heartbeat + edge counters). Built
    # unconditionally — it costs one more nested riscv build and its blob is only
    # linked when the application's CP_LP_BENCH switch references
    # cp_lp_bench_start(). Deliberately NOT a CMake option: a second switch that
    # could disagree with PlatformFeatures.h is exactly the drift this project
    # keeps paying for. Only one of the two programs is ever loaded at a time.
    set(ulp_app_name "ulp_cp_lp_bench")
    set(ulp_sources "ulp/main_bench.c")
    set(ulp_exp_dep_srcs "cp_lp_bench.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
    message(STATUS "cp_lp_core: building LP bench program ${ulp_app_name}")

    # r99: the THIRD program, the sentry (record wake sources / pre-filter the
    # power key). Same rules as the other two: built unconditionally, blob linked
    # only when CP_LP_BENCH or CP_LP_STANDBY references cp_lp_sentry_start().
    set(ulp_app_name "ulp_cp_lp_sentry")
    set(ulp_sources "ulp/main_sentry.c")
    set(ulp_exp_dep_srcs "cp_lp_sentry.c")
    ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
    message(STATUS "cp_lp_core: building LP sentry program ${ulp_app_name}")
else()
    message(STATUS "cp_lp_core: ULP co-processor not enabled (CONFIG_ULP_COPROC_ENABLED=n) — stubs only")
endif()

