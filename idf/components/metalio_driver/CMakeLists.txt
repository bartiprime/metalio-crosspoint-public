# Metalio E-Ink 4 Plus hardware driver (ESP32-S31).
#
# Holds the vendor EPDiy sources for this exact panel/board, verbatim from the
# vendor's own firmware tree. This is the display path proven on hardware:
#   epd_init(&epd_board_metalio_eink4_plus, &ED047TC2_1216, EPD_LUT_1K)
#   my_waveform_init() -> epd_hl_init(&MY_WAVEFORM) -> epd_poweron()
#   epd_clear() / epd_hl_update_screen(MODE_GC16)
#
# NOTE: esp32s31_region_protect_override.c is deliberately NOT part of this
# component. It exists only to neutralize the BROKEN PREBUILT IDF copy. This
# build uses from-source IDF, whose own esp_cpu_configure_region_protection()
# performs the correct PSRAM PMA mapping and locks entry 7. Adding the override
# here would suppress that mapping and break PSRAM.

file(GLOB_RECURSE MD_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c" "${CMAKE_CURRENT_LIST_DIR}/metalio_vendor_panel.cpp")

idf_component_register(
    SRCS ${MD_SRCS}
    INCLUDE_DIRS
        "."
        "display"
        "display/output_common"
        "display/output_lcd"
        "display/waveforms"
        "bus"
        "periph"
        "sleep"
    REQUIRES
        arduino
        driver
        esp_lcd
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_rmt
        esp_driver_dma
        esp_mm
        esp_psram
        esp_adc
        esp_hal_lcd
        esp_hal_rmt
        esp_hal_i2s
        esp_hal_regi2c
        esp_hw_support
        esp_timer
        esp_rom
        spi_flash
        esp_partition
)

# ---------------------------------------------------------------------------
# S31 light-sleep shim (sleep/metalio_sleep.c)
#
# Ported verbatim in behaviour from the vendor's power_policy/s31_sleep. Three
# --wrap hooks plus a project-local copy of the S31 PMU sleep source; the
# installed SDK is never edited. Without this, every light-sleep slice resumes
# with the boot analog bias (not the 12/12 the MPLL calibration needs) and with
# the 200 MHz octal PSRAM left off the MPLL clock source.
#
# https://github.com/espressif/esp-idf/commit/9726078b55430ef8d2ce4a460ebf21d889bb5c12
# https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/port/esp32s31/pmu_sleep.c
# ---------------------------------------------------------------------------
if(CONFIG_IDF_TARGET_ESP32S31)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_sleep_execute_event_callbacks"
        "-Wl,--wrap=pmu_sleep_start"
        "-Wl,--wrap=rtc_clk_mpll_configure"
    )

    set(_s31_pmu_path "$ENV{IDF_PATH}/components/esp_hw_support/port/esp32s31/pmu_sleep.c")
    if(EXISTS "${_s31_pmu_path}")
        file(READ "${_s31_pmu_path}" _s31_pmu)
        if(NOT _s31_pmu MATCHES "_psram_ctrlr_ll_select_clk_source")
            set(_s31_entry "s_mpll_freq_mhz_before_sleep = rtc_clk_mpll_get_freq();")
            set(_s31_entry_fix_body [=[
        if (s_mpll_freq_mhz_before_sleep) {
#if !BOOTLOADER_BUILD && CONFIG_SPIRAM
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_2, PSRAM_CLK_SRC_XTAL);
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_3, PSRAM_CLK_SRC_XTAL);
            if (!s_pmu_sleep_regdma_backup_enabled) {
                _psram_ctrlr_ll_enable_core_clock(PSRAM_CTRLR_LL_MSPI_ID_2, false);
                _psram_ctrlr_ll_enable_module_clock(PSRAM_CTRLR_LL_MSPI_ID_2, false);
            }
#endif
            rtc_clk_mpll_disable();
        }
]=])
            set(_s31_entry_fix "${_s31_entry}\n${_s31_entry_fix_body}")
            set(_s31_exit "rtc_clk_mpll_configure(clk_hal_xtal_get_freq_mhz(), s_mpll_freq_mhz_before_sleep, false);")
            set(_s31_exit_fix_body [=[
#if !BOOTLOADER_BUILD && CONFIG_SPIRAM
            if (!s_pmu_sleep_regdma_backup_enabled) {
                _psram_ctrlr_ll_enable_core_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
                _psram_ctrlr_ll_enable_module_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
            }
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_2, PSRAM_CLK_SRC_MPLL);
            _psram_ctrlr_ll_select_clk_source(PSRAM_CTRLR_LL_MSPI_ID_3, PSRAM_CLK_SRC_MPLL);
#endif
]=])
            set(_s31_exit_fix "${_s31_exit}\n${_s31_exit_fix_body}")
            string(FIND "${_s31_pmu}" "${_s31_entry}" _s31_entry_at)
            string(FIND "${_s31_pmu}" "${_s31_exit}" _s31_exit_at)
            if(_s31_entry_at LESS 0 OR _s31_exit_at LESS 0)
                message(FATAL_ERROR
                    "S31 PMU source changed: review the sleep clock handoff backport in "
                    "metalio_driver/CMakeLists.txt (anchors not found in ${_s31_pmu_path})")
            endif()
            string(REPLACE "${_s31_entry}" "${_s31_entry_fix}" _s31_pmu "${_s31_pmu}")
            string(REPLACE "${_s31_exit}" "${_s31_exit_fix}" _s31_pmu "${_s31_pmu}")
            file(WRITE "${CMAKE_BINARY_DIR}/s31_pmu_sleep_compat.c" "${_s31_pmu}")
            get_target_property(_s31_sources __idf_esp_hw_support SOURCES)
            list(FILTER _s31_sources EXCLUDE REGEX "port/esp32s31/pmu_sleep[.]c$")
            set_property(TARGET __idf_esp_hw_support PROPERTY SOURCES "${_s31_sources}")
            target_sources(__idf_esp_hw_support PRIVATE "${CMAKE_BINARY_DIR}/s31_pmu_sleep_compat.c")
            message(STATUS "metalio_driver: S31 PMU PSRAM clock handoff backport applied")
        else()
            message(STATUS "metalio_driver: S31 PMU already switches the PSRAM clock source; no backport")
        endif()
    else()
        message(WARNING "metalio_driver: S31 pmu_sleep.c not found at ${_s31_pmu_path}; sleep shim not patched")
    endif()
endif()

# Boot splash: one GC16 push right after the panel is powered and cleared, so the
# device shows something within ~2 s of power-on while the app finishes its own
# init. Product behaviour, not a diagnostic. -DCP_BOOT_SPLASH=OFF to disable.
option(CP_BOOT_SPLASH "Paint a boot splash once after panel init" OFF)
if(CP_BOOT_SPLASH)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_BOOT_SPLASH=1)
endif()

# r114d: CP_WAKE_RAW_CLEAR is no longer read by this component. The prime is now a function
# (metalio_vendor_panel_wake_prime in metalio_vendor_panel.cpp) called synchronously by the resume
# path, with its scan shape passed in at runtime — so there is nothing here to compile out. The
# option and its definition live on the app component (crosspoint_app/CMakeLists.txt), which is
# where the call site is. Kept as a comment, not an option, so one grep still explains the switch.

# Alignment calibration (edge bars + 100px ticks). BRING-UP ONLY: it draws over
# the UI and held the panel 20 s before the app could continue. Default OFF.
option(CP_PANEL_ALIGN_TEST "Draw alignment bars at driver init (bring-up only)" OFF)
if(CP_PANEL_ALIGN_TEST)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_PANEL_ALIGN_TEST=1)
endif()

# Peripheral probe passes ([periph:pre] before epd_init, [periph:post] after).
# BRING-UP ONLY: blocks the boot path on ~6 I2C devices and a 1-3 s audio reset.
option(CP_PERIPH_PROBE "Run the peripheral probe passes during boot" OFF)
if(CP_PERIPH_PROBE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_PERIPH_PROBE=1)
endif()

list(LENGTH MD_SRCS _n)
message(STATUS "metalio_driver: ${_n} vendor display sources")

# In-band stage signal: short frontlight pulses at stages that happen after GPIO19
# (EPD_PIN_MODE) is claimed, where the USB console is already dead and the RTC log
# buffer is not retained on this board. 1 = expander probe OK, 4 = probe failed,
# 2 = epd_poweron() returned. 120 ms pulses -- deliberately NOT the old multi-second
# ladder. Turn off with -DCP_STAGE_SIGNAL=OFF once the panel is confirmed working.
option(CP_STAGE_SIGNAL "Pulse the frontlight to report post-GPIO19 stages" OFF)
if(CP_STAGE_SIGNAL)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_STAGE_SIGNAL)
endif()

# Bring up the BH1098G Bluetooth-audio module during the boot probe (UART1
# TX39/RX38, AT+RX=2 then AT+MODE=1). Costs ~1 s and resets the module, so this is
# a bring-up switch: -DCP_AUDIO_PROBE=OFF once audio is confirmed.
#
# r162 AUDIT NOTE: the r162 hygiene audit listed CP_AUDIO_PROBE / CP_HAPTIC_PROBE
# as "emitted, read by nothing". That is wrong and this comment exists so the next
# audit does not re-open it: both are READ in metalio_vendor_panel.cpp (the
# periph:pre block, `#ifdef CP_AUDIO_PROBE` -> mn_audio_init(), `#ifdef
# CP_HAPTIC_PROBE` -> mn_haptic_pulse(50)). They are bring-up switches that are
# functionally complete and OFF by default, not dead defines. Both also sit inside
# `#if CP_PERIPH_PROBE`, so turning this one on without CP_PERIPH_PROBE=ON still
# does nothing -- that nesting is the only reason they read as inert.
option(CP_AUDIO_PROBE "Probe/initialise BH1098G audio during boot" OFF)
if(CP_AUDIO_PROBE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_AUDIO_PROBE=1)
endif()

# Boot self-test: one 50 ms pulse on the TCA P0.3 vibration motor. Keeps the haptic
# primitive in the image (otherwise dead-stripped with no caller) and gives the
# operator tactile confirmation at boot. -DCP_HAPTIC_PROBE=OFF to silence it.
option(CP_HAPTIC_PROBE "Pulse the vibration motor once at boot" OFF)
if(CP_HAPTIC_PROBE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_HAPTIC_PROBE=1)
endif()

# r97 Phase 2 — EPD_LUT_64K A/B. NOT a shipping change: default OFF keeps the 1 KB
# LUT this fork has always shipped, and nothing in HEAD turns this on. It exists so
# the LUT format can be measured in ONE scratch build instead of by editing
# display/metalio_epd.c:
#   idf.py -B ../build-lut64k -DMETALIO_EPD_LUT_64K=ON build
# What it can and cannot tell you (see display/metalio_epd.c and output_common/lut.c):
# diffs only for the 2 bpp modes (GC16/GL16 page turns); MODE_PACKING_1PPB_DIFFERENCE
# takes the 1K vector path on RENDER_METHOD_LCD regardless, and the extra 63 KB comes
# from INTERNAL RAM, not PSRAM.
option(METALIO_EPD_LUT_64K "Use the 64 KB epdiy waveform LUT (A/B experiment; default is the shipped 1 KB)" OFF)
if(METALIO_EPD_LUT_64K)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC METALIO_EPD_LUT_64K=1)
endif()
