# CrossPoint application + its libraries, compiled against FROM-SOURCE ESP-IDF.
#
# One component on purpose: upstream's sources, the freeink-sdk libraries and the
# third-party libraries are mutually dependent in both directions (headers and
# symbols), so splitting them into components would only create a REQUIRES web
# that has to be maintained. Includes are explicit, sources are globbed.
#
# NOTHING UPSTREAM IS EDITED. This file only points at ../../../src etc.

set(CP_ROOT        "${CMAKE_CURRENT_LIST_DIR}/../../..")          # crosspoint-reader/
set(CP_SRC         "${CP_ROOT}/src")
set(CP_SDK         "${CP_ROOT}/freeink-sdk")
set(CP_LIBDEPS     "${CP_ROOT}/.pio/libdeps/metalio_eink4_plus")

# ---- generated i18n tables --------------------------------------------------
# lib/I18n/I18nKeys.h, I18nStrings.h and I18nStrings.cpp are generated (and
# gitignored), so a clean checkout has no tables at all and the build used to
# fail on missing headers with nothing that could regenerate them. The script
# is run from the repository, never a copy:
#   * once here at configure time, because the source globs below only see the
#     files if they already exist;
#   * again as a build step keyed on the YAML, so edits regenerate without a
#     manual reconfigure. Only the YAML/script timestamps drive it, so an
#     unchanged tree does not recompile I18nStrings.cpp.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "crosspoint_app: Python 3 is required to generate lib/I18n (scripts/gen_i18n.py)")
endif()
set(CP_I18N_DIR "${CP_ROOT}/lib/I18n")
set(CP_I18N_OUTPUTS
    "${CP_I18N_DIR}/I18nKeys.h"
    "${CP_I18N_DIR}/I18nStrings.h"
    "${CP_I18N_DIR}/I18nStrings.cpp")
if(NOT EXISTS "${CP_I18N_DIR}/I18nStrings.cpp" OR NOT EXISTS "${CP_I18N_DIR}/I18nKeys.h")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CP_ROOT}/scripts/gen_i18n.py"
        WORKING_DIRECTORY "${CP_ROOT}"
        RESULT_VARIABLE _cp_i18n_rc)
    if(NOT _cp_i18n_rc EQUAL 0)
        message(FATAL_ERROR "crosspoint_app: scripts/gen_i18n.py failed (exit ${_cp_i18n_rc})")
    endif()
    message(STATUS "crosspoint_app: generated lib/I18n tables from translations")
endif()
file(GLOB CP_I18N_INPUTS "${CP_I18N_DIR}/translations/*.yaml")
# IDF also includes this file in script mode (-P, the requirements pass), where
# add_custom_command/add_custom_target are not scriptable. Only the real
# configure pass declares the build rule.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_command(
        OUTPUT ${CP_I18N_OUTPUTS}
        COMMAND "${Python3_EXECUTABLE}" "${CP_ROOT}/scripts/gen_i18n.py"
        WORKING_DIRECTORY "${CP_ROOT}"
        DEPENDS ${CP_I18N_INPUTS} "${CP_ROOT}/scripts/gen_i18n.py"
        COMMENT "Generating i18n tables (lib/I18n)"
        VERBATIM)
    add_custom_target(crosspoint_i18n DEPENDS ${CP_I18N_OUTPUTS})
endif()

file(GLOB_RECURSE CP_APP_SRCS "${CP_SRC}/*.cpp" "${CP_SRC}/*.c")
# (r97) The src/vendor_epd filter that stood here is gone: the shadow tree itself was deleted.
# The vendor display sources that actually run live in idf/components/metalio_driver.

# ---- freeink-sdk libraries (each lib/<area>/<name>) -------------------------
file(GLOB CP_SDK_LIB_DIRS LIST_DIRECTORIES true "${CP_SDK}/libs/*/*")

# ---- upstream's own libraries (crosspoint-reader/lib/<name>) ----------------
# These are part of the repository, not fetched, and the app's headers rely on
# them (BlePageTurner.h, HalStorage.h, PersistableStore.h, SdCardFontRegistry.h,
# GfxRenderer.h ...). Both the directory itself and its include/ and src/ are on
# the include path, which is how PlatformIO's lib layout resolves.
file(GLOB CP_LIB_DIRS LIST_DIRECTORIES true "${CP_ROOT}/lib/*")
set(CP_OWN_SRCS "")
set(CP_OWN_INC  "")
foreach(d ${CP_LIB_DIRS})
    if(NOT IS_DIRECTORY "${d}")
        continue()
    endif()
    list(APPEND CP_OWN_INC "${d}")
    foreach(sub include src)
        if(IS_DIRECTORY "${d}/${sub}")
            list(APPEND CP_OWN_INC "${d}/${sub}")
        endif()
    endforeach()
    file(GLOB_RECURSE _so "${d}/*.cpp" "${d}/*.c")
    # Test/bench/eample trees inside upstream libs need GoogleTest or host-only
    # scaffolding and are not part of the firmware. PlatformIO's library finder
    # skips them for the same reason.
    list(FILTER _so EXCLUDE REGEX "/(test|tests|examples|bench|benchmark)s?/")
    # (r97) The lib/EpdiyLcd filter that stood here is gone: the shadow copy of the vendor
    # display stack was deleted. idf/components/metalio_driver is the only copy that compiles.
    list(APPEND CP_OWN_SRCS ${_so})
endforeach()

set(CP_SDK_SRCS "")
set(CP_SDK_INC  "")
foreach(d ${CP_SDK_LIB_DIRS})
    if(IS_DIRECTORY "${d}/src")
        file(GLOB_RECURSE _s "${d}/src/*.cpp" "${d}/src/*.c")
        list(APPEND CP_SDK_SRCS ${_s})
    endif()
    if(IS_DIRECTORY "${d}/include")
        list(APPEND CP_SDK_INC "${d}/include")
    endif()
    if(IS_DIRECTORY "${d}/src")
        list(APPEND CP_SDK_INC "${d}/src")
    endif()
    # Bundled third-party code (freetype, stb, pngle, libunibreak, ...) keeps its
    # headers one level deeper: third_party/<name>/ and third_party/<name>/include/.
    if(IS_DIRECTORY "${d}/third_party")
        file(GLOB _tp LIST_DIRECTORIES true "${d}/third_party/*")
        foreach(t ${_tp})
            if(IS_DIRECTORY "${t}")
                list(APPEND CP_SDK_INC "${t}")
                if(IS_DIRECTORY "${t}/include")
                    list(APPEND CP_SDK_INC "${t}/include")
                endif()
            endif()
        endforeach()
    endif()
endforeach()

# ---- third-party libraries -------------------------------------------------
# Seven are VENDORED under idf/components/extern/ (src/ only, ~3 MB) so a fresh
# clone builds without running PlatformIO first. If a vendored copy is absent the
# .pio path is used as a fallback.
#
# Arduino-wolfSSL is the exception: its src/ is 45 MB and it is GPLv2-or-commercial,
# so it is NOT vendored. Fetch it once with:
#     pio pkg install -e metalio_eink4_plus
# (or point CROSSPOINT_WOLFSSL_DIR at an unpacked copy).
set(CP_EXT_VENDOR "${CMAKE_CURRENT_LIST_DIR}/../extern")
set(CP_EXT_NAMES SdFat ArduinoJson QRCode PNGdec JPEGDEC AnimatedGIF WebSockets)
set(CP_EXT_SRCS "")
set(CP_EXT_INC  "")
foreach(n ${CP_EXT_NAMES})
    if(IS_DIRECTORY "${CP_EXT_VENDOR}/${n}/src")
        set(_dir "${CP_EXT_VENDOR}/${n}")
    elseif(IS_DIRECTORY "${CP_LIBDEPS}/${n}/src")
        set(_dir "${CP_LIBDEPS}/${n}")
    else()
        message(FATAL_ERROR
            "Third-party library '${n}' not found.\n"
            "  expected: ${CP_EXT_VENDOR}/${n}/src  (vendored, preferred)\n"
            "  or:       ${CP_LIBDEPS}/${n}/src\n"
            "Run 'pio pkg install -e metalio_eink4_plus' to fetch it.")
    endif()
    file(GLOB_RECURSE _s "${_dir}/src/*.cpp" "${_dir}/src/*.c")
    list(APPEND CP_EXT_SRCS ${_s})
    list(APPEND CP_EXT_INC "${_dir}/src")
endforeach()

# wolfSSL: fetched, not vendored.
if(DEFINED ENV{CROSSPOINT_WOLFSSL_DIR})
    set(_wolf "$ENV{CROSSPOINT_WOLFSSL_DIR}")
else()
    set(_wolf "${CP_LIBDEPS}/Arduino-wolfSSL")
endif()

# C6: apply scripts/patch_wolfssl.py from THIS build too.
#
# The overrides (NO_DH off, FFDHE-2048 on, FP_MAX_BITS 8192, DEBUG_WOLFSSL off) used to
# be applied only by platformio.ini's `pre:` extra_script -- which an idf.py build never
# runs. They were reaching the tree only because a `pio pkg install` had once patched the
# cached package, so any fresh package (or a pio clean) silently dropped them, and the
# trace would come back with ~39 KB of flash. Running the same script here, at configure
# time, makes the shipped image independent of how the package was fetched.
#
# A failed patch is FATAL: a silently unpatched wolfSSL is a different firmware than the
# one that was measured, and nothing downstream would notice.
if(IS_DIRECTORY "${_wolf}/src")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CP_ROOT}/scripts/patch_wolfssl.py" "${_wolf}"
        RESULT_VARIABLE _wolf_patch_rc
        OUTPUT_VARIABLE _wolf_patch_out
        ERROR_VARIABLE _wolf_patch_err)
    if(NOT _wolf_patch_rc EQUAL 0)
        message(FATAL_ERROR "crosspoint_app: scripts/patch_wolfssl.py failed (rc=${_wolf_patch_rc}): ${_wolf_patch_err}")
    endif()
    file(READ "${_wolf}/src/user_settings.h" _wolf_settings)
    string(FIND "${_wolf_settings}" "CrossPoint wolfSSL compatibility overrides" _wolf_marker_at)
    if(_wolf_marker_at EQUAL -1)
        message(FATAL_ERROR "crosspoint_app: ${_wolf}/src/user_settings.h has no CrossPoint override block -- the patch did not land")
    endif()
    string(STRIP "${_wolf_patch_out}" _wolf_patch_out)
    message(STATUS "wolfSSL settings: ${_wolf_patch_out}")
endif()

if(IS_DIRECTORY "${_wolf}/src")
    file(GLOB_RECURSE _s "${_wolf}/src/*.cpp" "${_wolf}/src/*.c")
    # wolfSSL ships a large test/bench tree inside src/ on some releases; the
    # Arduino package does not, but keep the filter so a source drop behaves.
    list(FILTER _s EXCLUDE REGEX "/(test|tests)/")
    list(APPEND CP_EXT_SRCS ${_s})
    list(APPEND CP_EXT_INC "${_wolf}/src")
else()
    message(FATAL_ERROR
        "Arduino-wolfSSL not found at ${_wolf}/src.\n"
        "Run 'pio pkg install -e metalio_eink4_plus', or set CROSSPOINT_WOLFSSL_DIR.")
endif()

list(LENGTH CP_APP_SRCS _na)
list(LENGTH CP_OWN_SRCS _no)
list(LENGTH CP_SDK_SRCS _ns)
list(LENGTH CP_EXT_SRCS _ne)
message(STATUS "crosspoint_app: ${_na} app + ${_no} own-lib + ${_ns} sdk + ${_ne} third-party sources")

idf_component_register(
    SRCS "app_stub.cpp" ${CP_APP_SRCS} ${CP_OWN_SRCS} ${CP_SDK_SRCS} ${CP_EXT_SRCS}
    INCLUDE_DIRS
        "${CP_SRC}"
        "${CP_ROOT}"
        "${CP_ROOT}/compat/include"
        "${CP_SRC}/display"
        "${CP_SDK}/libs/network/BleKeyboardHost/include"
        ${CP_OWN_INC}
        ${CP_SDK_INC}
        ${CP_EXT_INC}
    REQUIRES
        arduino
        metalio_driver
        driver
        esp_lcd
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_uart
        esp_driver_rmt
        esp_driver_dma
        esp_driver_ppa
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_i2s
        esp_driver_jpeg
        esp_adc
        esp_mm
        bootloader_support
        esp_psram
        esp_hal_lcd
        esp_hal_rmt
        esp_hal_i2s
        esp_hw_support
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        fatfs
        spi_flash
        esp_partition
        app_update
        mbedtls
        efuse
        log
        console
        esp_driver_usb_serial_jtag
        cp_lp_core
        # r101.21 SIMD1: the `exp simd` s8 dot-product benchmark links esp-dsp's
        # own kernel against its _ansi twin. The component is already in the
        # build (idf/managed_components/espressif__esp-dsp -> libespressif__esp-dsp.a);
        # this only puts its public include dirs on crosspoint_app's path.
        esp-dsp
)

# The generated tables must exist before anything is compiled: a clean tree has
# no lib/I18n/I18n*.{h,cpp} until gen_i18n.py has run. (Guarded because IDF's
# script-mode requirements pass defines no command()/target() functions.)
if(TARGET crosspoint_i18n)
    add_dependencies(${COMPONENT_LIB} crosspoint_i18n)
endif()

# ---- AUDIT2: the unscaled-UI-literal table --------------------------------
# scripts/gen_ui_literals.py scans src/ for integer literals used where a UI metric is expected
# (band heights, row heights, control sizes) and writes lib/UiLiteralAudit/UiLiteralAudit.generated.h.
# The table is compiled in so main.cpp can report the class to /.crosspoint/debug.log at boot: this
# is the half of the scale audit that UITheme::auditUiScale() cannot do, because a literal is a
# metric that was never consulted (U-2's 50 px tab band, U-3's 28 px toggle -- both found on glass).
#
# Two deliberate properties:
#   * it regenerates as a normal build dependency, so a source edit cannot leave the table pointing
#     at yesterday's line numbers;
#   * it can never fail a build on findings. The generator always exits 0 -- reporting is not a gate,
#     and turning it into one is the operator's decision, not a side effect of an audit.
#
# Guarded out of script mode for the same reason as the i18n target above (IDF's requirements pass
# has no add_custom_command()/add_custom_target()).
set(CP_UILIT_OUT "${CP_ROOT}/lib/UiLiteralAudit/UiLiteralAudit.generated.h")
set(CP_UILIT_GEN "${CP_ROOT}/scripts/gen_ui_literals.py")
if(NOT EXISTS "${CP_UILIT_OUT}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CP_UILIT_GEN}"
        WORKING_DIRECTORY "${CP_ROOT}"
        RESULT_VARIABLE _cp_ulit_rc)
    if(NOT _cp_ulit_rc EQUAL 0)
        message(FATAL_ERROR "crosspoint_app: scripts/gen_ui_literals.py failed (exit ${_cp_ulit_rc})")
    endif()
    message(STATUS "crosspoint_app: generated lib/UiLiteralAudit table")
endif()
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE CP_UILIT_INPUTS "${CP_SRC}/*.cpp" "${CP_SRC}/*.h" "${CP_SRC}/*.hpp")
    add_custom_command(
        OUTPUT "${CP_UILIT_OUT}"
        COMMAND "${Python3_EXECUTABLE}" "${CP_UILIT_GEN}"
        WORKING_DIRECTORY "${CP_ROOT}"
        DEPENDS ${CP_UILIT_INPUTS} "${CP_UILIT_GEN}"
        COMMENT "Generating UI literal audit table (lib/UiLiteralAudit)"
        VERBATIM)
    add_custom_target(crosspoint_uilits DEPENDS "${CP_UILIT_OUT}")
    add_dependencies(${COMPONENT_LIB} crosspoint_uilits)
endif()

# ---- compile definitions, taken verbatim from platformio.ini ----------------
# The upstream build carries these in [base] and [env:metalio_eink4_plus]. They
# are not optional: USE_SEPARATE_FAT_CACHE changes SdFat's FAT cache behaviour,
# the WOLFSSL_* set selects the TLS configuration, and the XML_*/PNG_* limits are
# compile-time capacities used by the parsers.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CROSSPOINT_VERSION="MetalioEinkPlus r165.6"
    CP_USE_VENDOR_EPD=1
    # Compile the LOG_INF/LOG_DBG call sites so the Debug page's switch can enable them at runtime,
    # with no rebuild. logPrintf() drops them unless the switch is on, so the cost when off is one
    # branch. Flip this OFF for a lean production image; the Debug page then reports "compiled out".
    CP_DEBUG_LOG=1
    # Capture buffer for the SD log, in KB, allocated in PSRAM. 0 = no buffer and no flush path,
    # which is what a release image wants: build with -DCP_DEBUG_LOG=OFF -DCP_CAPTURE_LOG_KB=0 and
    # every diagnostic call site plus this buffer disappear at compile time.
    CP_CAPTURE_LOG_KB=64
    # Bring-up beacon ladder (freeink-sdk Epd047Tc2Driver/FreeInkDisplay) defaults to
    # CP_BEACON_FROM=1, i.e. EVERY stage strobes the frontlight and sleeps k seconds:
    # beacon(6) alone costs 300ms + 6s + 300ms + 1.5s = ~8.1s of dead time at panel
    # init, on every boot. Raise it past the highest stage for a production image.
    # -DCP_BOOT_BEACONS=ON to get the ladder back for a bring-up session.
    CP_BEACON_FROM=99
    # InputManager gates its expander volume-key hook on
    # `FREEINK_DEVICE_METALIO_EINK4 || defined(CP_INPUT_PLUS_FIX)`. This build defines
    # ..._EINK4_PLUS, so without this define the Plus has NO volume keys at all (the
    # file says so itself). CP_INPUT_PLUS_FIX is the escape hatch it provides.
    CP_INPUT_PLUS_FIX=1
    # 4 bpp canvas (anti-aliasing). ON is the shipping configuration for this
    # round: it is what makes a glyph's coverage survive to the panel nibble, and
    # it deletes the 1 bpp -> 4 bpp expand that used to cost 163-273 ms per
    # refresh. -DCP_RENDER_4BPP=OFF rebuilds the 1 bpp renderer; the 1 bpp path is
    # kept compiling on purpose so the two images can be A/B'd. See
    # lib/GfxRenderer/PixFormat.h and docs/engineering/ANTI_ALIASING_4BPP.md.
    CP_RENDER_4BPP=1
    # Touch mapping: the FT6336U's raw frame (landscape vs portrait) is decided by the
    # module, not the panel, and the vendor handles both. Enable the adaptive mapping so
    # taps are derived from the sample itself instead of a guessed axis convention.
    CP_TOUCH_ADAPTIVE=1
    # The panel is landscape-native (display path is the identity), so reader pages must
    # default to LANDSCAPE_CCW instead of the upstream PORTRAIT default.
    CP_DEFAULT_LANDSCAPE_READING=1
    ARDUINO_USB_MODE=1
    ARDUINO_USB_CDC_ON_BOOT=0
    BOARD_HAS_PSRAM
    CONTENT_EXTERNAL_MINIZ=1
    CP_CONSOLE_ONLY
    CP_LOG_VIA_PRINTF
    CROSSPOINT_WAIT_FOR_USB_SERIAL
    DESTRUCTOR_CLOSES_FILE=1
    DISABLE_FS_H_WARNING=1
    EINK_DISPLAY_SINGLE_BUFFER_MODE=1
    ENABLE_SERIAL_LOG
    # USB drive mode (scope C4): this board reaches mass storage through the
    # esp_tinyusb managed component's MSC class over the SdFat 512-byte sector
    # device, not through Arduino's USBMSC. It claims the USB pads only while the
    # screen is open, by throwing the ANA_SW switch (GPIO36) from the
    # USB-Serial/JTAG console to the OTG/HS-UTMI controller, and it gives them
    # back by rebooting. The console therefore stays on
    # CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG for the whole life of the image, which is
    # why CONFIG_TINYUSB_ENABLED is still absent (see idf/sdkconfig.defaults).
    #
    # FREEINK_CAP_USB_MSC stays 0: that macro belongs to the SDK's
    # Arduino-TinyUSB implementation (freeink-sdk libs/hardware/UsbMassStorage),
    # which needs CONFIG_TINYUSB_ENABLED and would take the console's peripheral.
    CP_USB_MSC=1
    FREEINK_CONTENT_WOLFSSL=1
    FREEINK_DEVICE_METALIO_EINK4_PLUS=1
    FREEINK_FONT_ENABLE_AUTOHINT=1
    FREEINK_NET_WOLFSSL=1
    HAVE_CURVE25519
    HAVE_FFDHE_2048
    HAVE_SNI
    MAX_WIDTH=3072
    NO_WOLFSSL_ESP32_CRYPT_RSA_PRI
    PNG_MAX_BUFFERED_PIXELS=16416
    USE_BLOCK_DEVICE_INTERFACE=1
    USE_SEPARATE_FAT_CACHE=1
    USE_SPI_ARRAY_TRANSFER=1
    USE_UTF8_LONG_NAMES=1
    WC_RC2
    WOLFSSL_CLIENT_EXAMPLE
    WOLFSSL_HAVE_SP_ECC
    WOLFSSL_KEY_GEN
    WOLFSSL_OPTIONS_H
    WOLFSSL_SP_SMALL
    WOLFSSL_TLS13
    WOLFSSL_USER_SETTINGS
    XML_CONTEXT_BYTES=0
    XML_GE=0
)

# C23 on this GCC makes `bool` a keyword, which breaks ricmoo/QRCode's
# `typedef unsigned char bool;`. Upstream fixes this with scripts/patch_qrcode.py,
# scoped to that one library. Same intent here: pin ONLY QRCode's sources to gnu17.
set_source_files_properties(${CP_EXT_SRCS} PROPERTIES COMPILE_OPTIONS "-std=gnu17")

# Bring-up bisection switch, off by default: set CP_NO_DISPLAY=1 in the environment
# to build an image that stops before display init so the USB serial console stays
# alive (GPIO19 / EPD_PIN_MODE shares USB D-).
# Explicit cache option, NOT an env var: an env-gated define gets baked into
# build.ninja and stays there after the variable is unset, silently shrinking the
# image by dropping the whole display path. Pass -DCP_NO_DISPLAY=ON to enable.
option(CP_NO_DISPLAY "Stop before display init so the USB console stays alive" OFF)
if(CP_NO_DISPLAY)
  message(STATUS "CP_NO_DISPLAY=ON -- stopping before display init")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_NO_DISPLAY)
endif()

# USB debug shell (r90): the esp_console REPL on the USB-Serial/JTAG console the
# logs already use -- phase 1 of docs/engineering/USB_DEBUG_CHANNEL.md. This
# switch is only "is the capability in the image"; whether it RUNS is the
# runtime setting SETTINGS.usbDebugShell (Debug page -> "USB debug shell").
# With -DCP_USB_SHELL=OFF the define is 0, src/debug/*.cpp compiles to stubs and
# no esp_console symbol is referenced (proved by symbol counts on both ELFs).
option(CP_USB_SHELL "Build the esp_console REPL on the USB-Serial/JTAG console" ON)
if(CP_USB_SHELL)
  message(STATUS "CP_USB_SHELL=ON -- esp_console REPL available (runtime toggle decides)")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_USB_SHELL=1)
else()
  message(STATUS "CP_USB_SHELL=OFF -- shell compiled out, esp_console not linked")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_USB_SHELL=0)
endif()

# ---- bench image switches (r97) ---------------------------------------------
# All five accelerator/bench switches in src/platform/PlatformFeatures.h default to 0,
# so a plain build of this tree is the SHIPPING image. The r96/r97 measurement image is
# selected here, in one place, instead of by hand-editing that header:
#
#     idf.py -B ../build-bench -DCP_BENCH=ON build
#
# CP_BENCH=ON defines all five to 1. PlatformFeatures.h guards each with #ifndef, so a
# -D wins over the file's default without the file being touched -- which means the two
# artifacts differ only in these switches and a size/string comparison between them is
# meaningful. OFF (the default) is what HEAD means: no ULP reference, no ppa_* symbol,
# no `cpu`/`pm`/`lpbench`/`lpsleep`/`ppabench` commands, no run-time stats reader.
option(CP_BENCH "Build the measurement image (LP core, PPA, CPU stats, LP/PPA bench)" OFF)
if(CP_BENCH)
  message(STATUS "CP_BENCH=ON -- BENCH image: CP_LP_CORE/CP_PPA/CP_CPU_STATS/CP_LP_BENCH/CP_PPA_BENCH = 1")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC
      CP_LP_CORE=1
      CP_PPA=1
      CP_CPU_STATS=1
      CP_LP_BENCH=1
      CP_PPA_BENCH=1)
else()
  message(STATUS "CP_BENCH=OFF -- SHIPPING image: all five accelerator/bench switches 0")
endif()

# ---- CP_LP_STANDBY (r99) ----------------------------------------------------
# The one switch that is NOT part of CP_BENCH: it changes the STANDBY path, not a
# measurement, and a bench image is not where it should be trialled by accident.
# Default OFF; the shipping image is the default image.
#
#     idf.py -B ../build-lp -DCP_LP_STANDBY=ON build
#
# With it ON the standby loop keeps the HP core asleep and lets the LP core
# pre-filter the power key (src/main.cpp metalioStandbyLoop, and the full
# rationale in src/platform/PlatformFeatures.h beside the switch).
option(CP_LP_STANDBY "Standby: LP core pre-filters the power key (HP does not arm EXT1)" OFF)
if(CP_LP_STANDBY)
  message(STATUS "CP_LP_STANDBY=ON -- the LP sentry gates standby wake on GPIO2")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_LP_STANDBY=1)
else()
  message(STATUS "CP_LP_STANDBY=OFF -- standby unchanged (EXT1 on GPIO2 + 60 s timer)")
endif()

# ---- standby A/B trims (r98.1, DEFAULT ON since r99.1) ------------------------
# CP_CUT_PA_RAIL_ON_STANDBY / CP_SD_PARK_ON_STANDBY: both default ON from r99.1.
# They were built as OFF-by-default MEASUREMENT arms until round 3 on glass closed
# the measurement, which is why they are now shipped behaviour:
#
#   round 3, 3 Nov 2026 (WAKE2/PWR3 A/B, same battery state, same soak):
#     10.38 mA with both trims on vs 17.53 mA untrimmed -- a 40.7 % cut, which
#     moves standby endurance from 4.75 to 8.01 days on the 2000 mAh pack.
#     Evidence: 7/7 parks armed, ZERO resets in the whole run, SD remount
#     68-69 ms inside the 250 ms settle budget.
#
# The runtime override machinery is deliberately untouched and still WINS over this
# default: Debug -> "Arm PA-rail trim" / "Arm SD-park trim", or the shell's
# `trim pa on|off` / `trim sd on|off`, can turn either trim OFF for the next
# standby without a rebuild. That is the only way to run the WAKE2 A/B (spurious
# wake counts, trims ON vs OFF alternating) on a stock image, so do not "simplify"
# it away. `trim status` reports compiled=ON / effective=... precisely so a run can
# never be attributed to a trim that was not in the image.
#
# src/display/RefreshPolicy.h guards them with #ifndef/#define 0, so until r98.1
# `-D<name>=1` on the idf.py command line created only a CMake cache variable -- no
# -D reached the compiler, the file's #ifndef default held, and an "A/B" built that
# way compared two identical images.
#
# The option()s for these two live in idf/CMakeLists.txt (before project(), beside
# CP_BENCH); the option()s below are no-ops over those cache entries for the same
# reason CP_BENCH's is. This is the half that does the work: the compile definition.
#
#     idf.py -B ../build-pa -DCP_CUT_PA_RAIL_ON_STANDBY=ON build
#
# The definitions are emitted ON (1) *and* OFF (0) on purpose: a build log then
# states the value rather than leaving it to an #ifndef fallback, and one grep of
# the compile flags answers "was this image trimmed or not".
option(CP_CUT_PA_RAIL_ON_STANDBY "Standby: hold TCA P1.4 (BT/PA rail) low for the whole standby" ON)
option(CP_CUT_CAM_RAIL_ON_STANDBY "Standby: hold TCA P0.5 (CAM_SCR camera/screen rail) low for the whole standby" OFF)
option(CP_SD_PARK_ON_STANDBY "Standby: unmount and park the SD card for the whole standby" ON)
# CP_AUTOFLUSH_ON_STANDBY (r98.3): write the in-RAM log ring to /.crosspoint/debug.log at every
# park and resume, so `logtail` shows the real state after a sleep without the operator pressing
# Debug -> "Print logs to SD". Exists to make the standby A/B readable from the card; safe to drop
# once the wake-time recorder is trusted.
option(CP_AUTOFLUSH_ON_STANDBY "Standby: flush the log ring + journal to SD at park and resume" ON)
# r113: the r112 F1' settle deadline (CP_WAKE_SETTLE_MS) was removed on device evidence
# (1500 ms did not change the banding; the sleep-image drive is clean). No definition is emitted.
# r116: CP_WAKE_FULL_SPEED is REMOVED (source deleted, PM compiled out). Nothing is emitted for
# it, so a stale -DCP_WAKE_FULL_SPEED=ON on a command line cannot creep back into an image.
# r119: CP_WAKE_SETTLE_EXTRA_MS — extra dead time, in ms, between the resume autoflush and the
# first content drive (src/main.cpp spends it as delay(), before the wake prime). 0 = no wait.
# The macro existed only as an `#ifndef` floor in src/platform/PlatformFeatures.h since r96.3, so
# `-DCP_WAKE_SETTLE_EXTRA_MS=800` created a CACHE VARIABLE and reached no compiler: the header
# default won and every "800 ms" A/B measured a 0 ms image. That is the trap documented at
# idf/CMakeLists.txt:200-215. The definition below is the half that reaches the compiler.
if(NOT DEFINED CP_WAKE_SETTLE_EXTRA_MS)
  set(CP_WAKE_SETTLE_EXTRA_MS 0 CACHE STRING "Resume: extra dead time before the first content drive, ms (default 0)")
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_WAKE_SETTLE_EXTRA_MS=${CP_WAKE_SETTLE_EXTRA_MS})
message(STATUS "CP wake settle extra: CP_WAKE_SETTLE_EXTRA_MS=${CP_WAKE_SETTLE_EXTRA_MS} (ms after the resume autoflush)")

# r129 (H23): CP_LOG_HOLD_DURING_DRIVE — hold the SD log drain for the duration of every panel
# waveform (vendor parity: epd_refresh.cc:240-249 SdLogWriter_LockForDisplayRefresh). The panel shim
# calls the drive-gate hook at the entry and exit edges of each drive and src/main.cpp answers with
# logSetDriveActive(), which only DEFERS the drain that logSdFlush() was about to do. Nothing is
# dropped and no ordering changes: the ring is append-only, so the block drains in log order as soon
# as the waveform returns.
# Same trap as CP_WAKE_SETTLE_EXTRA_MS above: the macro is an `#ifndef` floor in
# src/platform/PlatformFeatures.h, so a bare -DCP_LOG_HOLD_DURING_DRIVE=1 would set a CMake cache
# variable and reach no compiler. The definition below is the half that reaches it.
option(CP_LOG_HOLD_DURING_DRIVE "Hold the SD log drain for the duration of every panel waveform" ON)
if(CP_LOG_HOLD_DURING_DRIVE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_LOG_HOLD_DURING_DRIVE=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_LOG_HOLD_DURING_DRIVE=0)
endif()
message(STATUS "CP log hold during drive: CP_LOG_HOLD_DURING_DRIVE=${CP_LOG_HOLD_DURING_DRIVE}")

# ---- r156: CP_UI_GREY_CLEAN_EVERY (B1, the AA-ghosting fix) ------------------
# A global UI grey-paint cadence: every N paints that go through the panel's
# MULTI-LEVEL (grey / 2-bit AA) lane, the next one is driven as the existing
# from-white GC16 clean (idf/components/metalio_driver/metalio_vendor_panel.cpp,
# `greyCadenceDue` -> `epd_hl_update_screen_from_white(MODE_GC16)` -- the same
# drive the vendor-parity `cadenceDue` already uses, not a second mechanism).
#
# WHY. The reader already has this cadence for page turns (SETTINGS
# getRefreshFrequency -> ReaderUtils::displayWithRefreshCycle forces a FULL every
# N pages), and the vendor manages ghosting the same way ("Clean every %d" in
# RickyOS.bin) rather than by dithering text. Menus and other activities paint
# 2-bit AA text with NO such counter, so their grey residue accumulated with
# nothing to discharge it -- the ghosting seen once AA started working.
#
# N is a NUMBER, so this follows the CP_WAKE_SETTLE_EXTRA_MS precedent (CACHE
# STRING, emitted verbatim) rather than the ON/OFF option() convention: the
# definition is emitted on BOTH branches -- 1 and 0 are just two of the values --
# so the configure log and compile_commands.json always state the number that
# shipped instead of leaving it to the #ifndef floor in the .cpp.
#
# 0 turns the cadence off (r155 behaviour at that site, no rebuild of the code).
#
#     idf.py -B ../build-r156 build                     (N=5, the shipping value)
#     idf.py -B ../build-grey2 -DCP_UI_GREY_CLEAN_EVERY=2 build
#
# The counter lives in the metalio_driver component (that is the only layer that
# knows whether a paint actually took the grey lane: `useGray8 = !clean && src4bpp
# && frameHasGrey`), so the definition is emitted on that target as well. A single
# compile definition on crosspoint_app would NOT reach it -- component PUBLIC
# definitions do not flow against the REQUIRES direction.
if(NOT DEFINED CP_UI_GREY_CLEAN_EVERY)
  set(CP_UI_GREY_CLEAN_EVERY 5 CACHE STRING
      "UI grey-paint cadence: force a from-white GC16 clean every N grey-lane paints (0 = off, default 5)")
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_UI_GREY_CLEAN_EVERY=${CP_UI_GREY_CLEAN_EVERY})
if(TARGET __idf_metalio_driver)
  target_compile_definitions(__idf_metalio_driver PUBLIC CP_UI_GREY_CLEAN_EVERY=${CP_UI_GREY_CLEAN_EVERY})
else()
  message(FATAL_ERROR
      "CP_UI_GREY_CLEAN_EVERY: the __idf_metalio_driver target does not exist, so the "
      "grey-paint counter in metalio_vendor_panel.cpp would silently compile its #ifndef "
      "fallback and the -D value would never reach the compiler. Fix the component "
      "registration rather than removing this check.")
endif()
message(STATUS "CP UI grey clean every: CP_UI_GREY_CLEAN_EVERY=${CP_UI_GREY_CLEAN_EVERY} (N grey-lane paints between from-white GC16 cleans; 0 = cadence off)")

# ---- r161/r162: CP_READER_AA_LANE (DEFAULT OFF again in r162) ---------------
# OFF SHIPS. With it off a reader PAGE TURN requests GL16, which the panel drives
# as epd_hl_update_screen(MODE_GL16): a diff drive with no wipe prefix. That is
# r160's behaviour, and it is what r162 restores.
#
# With it ON (the r161 experiment) the turn requests FAST and the frame picks the
# lane in metalio_vendor_panel.cpp. On a 4 bpp canvas with covered glyphs that
# ALWAYS resolved to `useGray8` -> my_waveform_wipe_gl(3), 38 phases, whose first
# phase drives the panel to paper: the page-turn white flash, plus a
# CP_UI_GREY_CLEAN_EVERY slot per turn (every 5th turn a from-white clean).
#
# The reader's Text AA setting is NOT this switch any more. `textAntiAliasing`
# drives the glyph blitter only (`renderer.setTextBinary`, applied per paint in
# ActivityManager) on top of whichever request this lane selects, so ON/OFF
# changes the glyphs and never the drive.
option(CP_READER_AA_LANE "Reader page turns request FAST so the frame picks the grey lane (r161 A/B; OFF ships)" OFF)
if(CP_READER_AA_LANE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_READER_AA_LANE=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_READER_AA_LANE=0)
endif()
message(STATUS "CP reader AA lane: CP_READER_AA_LANE=${CP_READER_AA_LANE} (OFF ships: page turns request GL16; ON = r161 A/B, page turns request FAST)")

# ---------------------------------------------------------------------------
# CP_PARK_WIFI_PAUSE (r117) — BUCKET B, DEFAULT OFF. Vendor power policy stops the
# radio driver (esp_wifi_stop) BEFORE the station is de-initialised, so the driver's
# timers/beacon-tracking are not left running into the park. It changes what is
# POWERED during the park, so it is deferred until the park reorder is confirmed on
# glass. Compiled out entirely at 0 (ParkSinks::radioDown has no body for it).
# ---------------------------------------------------------------------------
option(CP_PARK_WIFI_PAUSE "Standby park: esp_wifi_stop() before the radio sink de-inits the stack (bucket B, default OFF)" OFF)
if(CP_PARK_WIFI_PAUSE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_PARK_WIFI_PAUSE=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_PARK_WIFI_PAUSE=0)
endif()
message(STATUS "CP park wifi pause: CP_PARK_WIFI_PAUSE=${CP_PARK_WIFI_PAUSE} (bucket B)")
# r128: CP_WAKE_ONE_DRIVE_STRICT is not emitted either: the row-9 completion rule is r104's, and
# the switch's floor in src/platform/PlatformFeatures.h (0) is what this image compiles.
if(CP_AUTOFLUSH_ON_STANDBY)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_AUTOFLUSH_ON_STANDBY=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_AUTOFLUSH_ON_STANDBY=0)
endif()
message(STATUS "CP standby autoflush: CP_AUTOFLUSH_ON_STANDBY=${CP_AUTOFLUSH_ON_STANDBY}")
if(CP_CUT_PA_RAIL_ON_STANDBY)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_CUT_PA_RAIL_ON_STANDBY=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_CUT_PA_RAIL_ON_STANDBY=0)
endif()
if(CP_CUT_CAM_RAIL_ON_STANDBY)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_CUT_CAM_RAIL_ON_STANDBY=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_CUT_CAM_RAIL_ON_STANDBY=0)
endif()
if(CP_SD_PARK_ON_STANDBY)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_SD_PARK_ON_STANDBY=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_SD_PARK_ON_STANDBY=0)
endif()
# One line, always printed, naming which trims are ON -- so every build log states
# the configuration of the image it produced.
if(CP_CUT_PA_RAIL_ON_STANDBY AND CP_CUT_CAM_RAIL_ON_STANDBY AND CP_SD_PARK_ON_STANDBY)
  message(STATUS "CP standby trims: CP_CUT_PA_RAIL_ON_STANDBY=ON CP_CUT_CAM_RAIL_ON_STANDBY=ON CP_SD_PARK_ON_STANDBY=ON (SHIPPING configuration; runtime `trim pa|cam|sd off` still overrides)")
else()
  message(STATUS "CP standby trims: CP_CUT_PA_RAIL_ON_STANDBY=${CP_CUT_PA_RAIL_ON_STANDBY} CP_CUT_CAM_RAIL_ON_STANDBY=${CP_CUT_CAM_RAIL_ON_STANDBY} CP_SD_PARK_ON_STANDBY=${CP_SD_PARK_ON_STANDBY} (A/B MEASUREMENT IMAGE -- at least one trim compiled out)")
endif()

# ---- CP_ICON_PARTIAL_REPAINT (r98) ------------------------------------------
# The battery charging glyph repaints its own region when isCharging() flips, so
# the operator no longer has to navigate away and back to see the bolt. Default
# ON; pure improvement, revertible.
#
#     idf.py -B ../build-icon build                        (ON, the default)
#     idf.py -B ../build-icon -DCP_ICON_PARTIAL_REPAINT=OFF build   (r97.2 behaviour)
#
# THE HALF THAT MATTERS: the definition. The option() here is a no-op over the
# cache entry created in idf/CMakeLists.txt (before project()), which is where
# `-D<name>` has to land for anything but crosspoint_app to see it; this is the
# half that actually reaches the compiler, because src/display/RefreshPolicy.h
# guards the switch with #ifndef and a header default alone is NOT settable from
# the build (the same blocker that bit CP_CUT_PA_RAIL_ON_STANDBY /
# CP_SD_PARK_ON_STANDBY -- see the block above).
#
# Both branches emit the definition (1 and 0) on purpose: one grep of the compile
# flags then answers whether a given image has the path in or out, instead of
# leaving it to an #ifndef fallback that can differ from the build log.
option(CP_ICON_PARTIAL_REPAINT "Battery icon: repaint only the icon's region when isCharging() flips" ON)

# ---- r101.21: CP_RICKY_COMPLETION_DRIVE + CP_EXP (the definition half) ------
# The option()s live in idf/CMakeLists.txt (before project()); these are no-ops
# over those cache entries, and this is the half that reaches the compiler. Both
# branches emit the value (1 and 0) so the build log states it rather than
# leaving it to an #ifndef fallback.
option(CP_RICKY_COMPLETION_DRIVE "MAIN: append a row-9 GL16 completion drive after a FAST paint" ON)
if(CP_RICKY_COMPLETION_DRIVE)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_RICKY_COMPLETION_DRIVE=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_RICKY_COMPLETION_DRIVE=0)
endif()
option(CP_EXP "EXP1: compile the `exp` runtime arm substrate (all arms default to today's behaviour)" ON)
if(CP_EXP)
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_EXP=1)
  message(STATUS "EXP1: CP_EXP=ON -- `exp` arm substrate compiled in, every arm at its r101.20 default")
else()
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_EXP=0)
  message(STATUS "EXP1: CP_EXP=OFF -- identity control: arm table, `exp` command and Debug rows compiled out")
endif()
message(STATUS "EDRV1: CP_RICKY_COMPLETION_DRIVE=${CP_RICKY_COMPLETION_DRIVE}")
if(CP_ICON_PARTIAL_REPAINT)
  message(STATUS "CP_ICON_PARTIAL_REPAINT=ON -- charging glyph repaints its own region (DU, mask-limited)")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_ICON_PARTIAL_REPAINT=1)
else()
  message(STATUS "CP_ICON_PARTIAL_REPAINT=OFF -- no icon repaint path; r97.2 behaviour at that call site")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_ICON_PARTIAL_REPAINT=0)
endif()

# ---- CP_ICON_SAFE_PATCH (BATT5) --------------------------------------------
# The REPLACEMENT for the switch above: the same glyph rectangle, the same DU drive,
# but licensed by a positive fact (the renderer's compose epoch == its glass epoch,
# lib/GfxRenderer/GlassEpoch.h) instead of by "a rect was recorded". It runs on every
# screen INCLUDING the reader, where a rect patch costs no page re-render.
#
#     idf.py -B ../build-r165 build                          (ON, the default)
#     idf.py -B ../build-r165 -DCP_ICON_SAFE_PATCH=OFF build  (r164 behaviour exactly)
#
# CP_ICON_SAFE_PATCH=ON forces CP_ICON_PARTIAL_REPAINT to 0 in
# src/display/RefreshPolicy.h, so the retired self-repaint cannot be resurrected
# alongside it by a stale -D. The definitions are emitted explicitly (1 and 0) so one
# grep of the compile flags answers which producer a given image carries.
option(CP_ICON_SAFE_PATCH "Battery icon: guarded region patch, licensed by framebuffer==glass" ON)
if(CP_ICON_SAFE_PATCH)
  message(STATUS "CP_ICON_SAFE_PATCH=ON -- charging glyph patches its rect, licensed by canvas==glass (all screens)")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_ICON_SAFE_PATCH=1)
else()
  message(STATUS "CP_ICON_SAFE_PATCH=OFF -- no region patch; whole-activity DU repaint only (r164 behaviour)")
  target_compile_definitions(${COMPONENT_LIB} PUBLIC CP_ICON_SAFE_PATCH=0)
endif()

# ---- eFuse safety gate (r97) ------------------------------------------------
# Operator standing rule: the eFuse is READ-ONLY forever; no code path writes it.
# A burn is irreversible, and the edit that would cause one is a one-line Kconfig
# change, so the rule is enforced by the BUILD rather than by documentation.
# tools/check_efuse_safety.py fails when (a) any eFuse write/burn API is
# referenced in src/, lib/, idf/components/ or freeink-sdk/, or (b)
# CONFIG_SECURE_BOOT=y / CONFIG_SECURE_FLASH_ENC_ENABLED=y appears in
# idf/sdkconfig or idf/sdkconfig.defaults. The target has no OUTPUT, so it runs on
# EVERY build, and the app component depends on it, so it runs before anything is
# compiled. There is no flag to skip it on purpose: if it fires, the fix is in the
# tree, not on the command line.
if(NOT CMAKE_SCRIPT_MODE_FILE)
  set(CP_EFUSE_GATE_SCRIPT "${CP_ROOT}/tools/check_efuse_safety.py")
  add_custom_target(crosspoint_efuse_gate ALL
      COMMAND "${Python3_EXECUTABLE}" "${CP_EFUSE_GATE_SCRIPT}" --root "${CP_ROOT}"
      WORKING_DIRECTORY "${CP_ROOT}"
      COMMENT "eFuse safety gate: no write API in our sources, secure boot / flash enc off"
      VERBATIM)
  add_dependencies(${COMPONENT_LIB} crosspoint_efuse_gate)
endif()
