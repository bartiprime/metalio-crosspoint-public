cmake_minimum_required(VERSION 3.16.0)

# CrossPoint Reader — Metalio E-Ink 4 Plus (ESP32-S31), Path A.
#
# WHY THIS DIRECTORY EXISTS
#   Upstream CrossPoint is a PlatformIO/Arduino project. PlatformIO links the
#   Arduino core's PREBUILT IDF archives (framework-arduinoespressif32-libs),
#   and one of those archives leaves PMA entry 7 unmapped, so PSRAM is
#   registered with the heap but unusable. Verified on this board:
#       E esp_image: Image length 4471216 doesn't fit in partition length 1048576
#       (and, before that, silent reset loops with a half-mapped PSRAM window)
#
#   This project builds the SAME Arduino application code, but with ESP-IDF
#   compiled FROM SOURCE (M:/cpc/packages/framework-espidf, esp32s31 target),
#   where esp_cpu_configure_region_protection() performs the PSRAM PMA write:
#       csrwi 0xbc7,0 / csrwi 0xbd7,0 / csrw 0xbd7,0x14800000 / csrw 0xbc7,0xe000001d
#   Proven on hardware by probe_arduino/ (glass: full black -> full white).
#
# UPSTREAM-CLEAN RULE
#   Everything this board needs lives under this directory. Nothing upstream is
#   edited, so `git pull upstream main` stays a fast-forward. Application code
#   is only REFERENCED (../../src), never moved or patched.
#
# DO NOT add esp32s31_region_protect_override.c here. The from-source IDF tree
# already carries the patched, correct implementation; our override is an EMPTY
# function that exists only to neutralize the broken PREBUILT copy. Adding it to
# this build would suppress the working PMA mapping and break PSRAM again.

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Windows caps a CreateProcess command line at 8191 characters. With ~60 include
# directories plus the compile definitions, a single crosspoint_app translation
# unit exceeds it and ninja dies with:
#     ninja: fatal: CreateProcess: The parameter is incorrect.
# Response files move the includes/defines/objects out of the command line into
# @file arguments. Must be set BEFORE project().
set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES  ON)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES ON)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_DEFINES   ON)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_DEFINES  ON)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS   ON)
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS ON)
# The switches above are not enough: with IDF's generator the compile command was
# still emitted inline at 29,958 characters (350 includes + 49 defines), and
# ninja died with "CreateProcess: The parameter is incorrect". This forces a
# response file for EVERY rule, which is the only reliable way to stay under
# Windows' 8191-character limit.
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)

# ---------------------------------------------------------------------------
# COMPONENT-LEVEL SWITCHES — must exist BEFORE project().
#
# These two options decide whether idf/components/cp_lp_core contributes any
# source at all (it adds ULP programs, which are a nested riscv build). A
# compile definition on crosspoint_app is too late and too narrow for that: the
# component is configured first, and a definition does not reach CMake. So the
# options live here, in the project file, where every component can read them.
#
# crosspoint_app still owns the same names as compile definitions
# (idf/components/crosspoint_app/CMakeLists.txt) — an option() over an existing
# cache entry is a no-op, so the two cannot disagree.
#
#   CP_BENCH=ON       -> the measurement image (LP core, PPA, CPU stats, benches)
#   CP_LP_STANDBY=ON  -> the r99 standby path: the LP core pre-filters the power
#                        key. NOT part of CP_BENCH: it changes standby, not a
#                        measurement.
#
# With both OFF the LP component registers zero sources: the shipping image is
# r97.1's, and no ULP binary is built at all.
# ---------------------------------------------------------------------------
option(CP_BENCH "Build the measurement image (LP core, PPA, CPU stats, LP/PPA bench)" OFF)
option(CP_LP_STANDBY "Standby: LP core pre-filters the power key (HP does not arm EXT1)" OFF)

# ---------------------------------------------------------------------------
# STANDBY A/B TRIMS — must exist BEFORE project().
#
# WHY THESE LIVE HERE AND NOT ONLY IN crosspoint_app/CMakeLists.txt
#   src/display/RefreshPolicy.h guards both switches with
#       #ifndef CP_CUT_PA_RAIL_ON_STANDBY
#       #define CP_CUT_PA_RAIL_ON_STANDBY 0
#   and nothing in the build defined them. `idf.py -DCP_CUT_PA_RAIL_ON_STANDBY=1`
#   creates a CMake CACHE VARIABLE and nothing else: no -D ever reached the
#   compiler, the #ifndef default held at 0, and an A/B built that way would have
#   measured two identical images while appearing to work. That is a measurement
#   that silently measures nothing, which is worse than one that fails to build.
#
#   These option()s pair with the compile definitions in
#   idf/components/crosspoint_app/CMakeLists.txt (same names, same "option() over
#   an existing cache entry is a no-op" rule as CP_BENCH), so `-D<name>=ON` on the
#   idf.py command line now sets the cache entry HERE and the definition THERE.
#   Both default ON from r99.1: the trims are shipped behaviour now, because round 3
#   on glass measured 10.38 mA trimmed vs 17.53 mA untrimmed (-40.7 %, standby
#   4.75 -> 8.01 days) with 7/7 parks armed, zero resets and SD remount 68-69 ms
#   inside the 250 ms budget. `-D<name>=OFF` still builds a single-arm A/B image,
#   and the runtime overrides (`trim pa|sd on|off`, the Debug arm rows) still force
#   either trim off without a rebuild.
#
#   CP_CUT_PA_RAIL_ON_STANDBY -> TCA P1.4 (BT_PA_PWR) low for the whole standby,
#                                re-asserted before the panel is re-powered.
#   CP_CUT_CAM_RAIL_ON_STANDBY -> TCA P0.5 (CAM_SCR_EN) low for the whole standby,
#                                re-asserted AFTER P1.4 has settled (vendor order).
#   CP_SD_PARK_ON_STANDBY     -> unmount + park the SD card for the standby,
#                                remount on resume before anything touches storage.
# ---------------------------------------------------------------------------
option(CP_CUT_PA_RAIL_ON_STANDBY "Standby: hold TCA P1.4 (BT/PA rail) low for the whole standby" ON)
option(CP_CUT_CAM_RAIL_ON_STANDBY "Standby: hold TCA P0.5 (CAM_SCR camera/screen rail) low for the whole standby" OFF)
option(CP_SD_PARK_ON_STANDBY "Standby: unmount and park the SD card for the whole standby" ON)

# r113: CP_WAKE_SETTLE_MS (r112 F1') was REMOVED. The settle deadline assumed the first
# post-park drive bands because the panel is not settled; on device 1500 ms changed nothing and
# the sleep-image drive (on an already-powered panel) is clean. The wake path now only LOGS the
# observed settle - no wait is imposed, so no wake latency is added for no benefit.

# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# CP_WAKE_FULL_SPEED (r115) is REMOVED in r116 — see src/platform/PlatformFeatures.h. The
# resume clock window and its esp_pm locks are gone from source, and PM itself is off again.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# CP_ICON_PARTIAL_REPAINT — the battery charging glyph repaints itself (r98).
#
# Default ON: it is a pure improvement and fully revertible (-DCP_ICON_PARTIAL_REPAINT=OFF
# reproduces r97.2's behaviour at that call site byte for byte -- see the switch's
# documentation in src/display/RefreshPolicy.h).
#
# WHY THE OPTION EXISTS HERE AS WELL AS IN crosspoint_app/CMakeLists.txt
#   src/display/RefreshPolicy.h guards it with `#ifndef ... #define 1`, and a #define
#   alone is NOT settable from the build: the same blocker that bit the standby
#   switches (see the block above). `idf.py -DCP_ICON_PARTIAL_REPAINT=OFF` creates a
#   CMake CACHE VARIABLE and nothing else; without an option() HERE the cache entry
#   is never created at project level either, and the compile definition in
#   crosspoint_app/CMakeLists.txt is the half that actually reaches the compiler.
#   The two agree by construction: option() over an existing cache entry is a no-op.
# ---------------------------------------------------------------------------
option(CP_ICON_PARTIAL_REPAINT "Battery icon: repaint only the icon's region when isCharging() flips" ON)

# ---------------------------------------------------------------------------
# CP_RICKY_COMPLETION_DRIVE — the row-9 completion drive after a FAST paint.
#
# r101.21 (EDRV1/ARMW0) adds the MISSING option() for a macro that has existed
# only as an `#ifndef` default in src/display/RefreshPolicy.h:34-35 since r96.
# Until this existed, `-DCP_RICKY_COMPLETION_DRIVE=0` created a CMake cache
# variable and reached no compiler: every A/B quoted as "behind the existing
# CP_RICKY_COMPLETION_DRIVE=0 switch" measured two identical images. That is the
# exact trap documented at :78-96 above.
#
# Default ON = the value the header default has always had, so the shipping
# image is unchanged; only addressability changes. The definition below emits 1
# or 0 explicitly, so one grep of the compile flags answers whether a given
# image carries the drive.
# ---------------------------------------------------------------------------
option(CP_RICKY_COMPLETION_DRIVE "MAIN: append a row-9 GL16 completion drive after a FAST paint" ON)

# ---------------------------------------------------------------------------
# CP_EXP — EXP1, the `exp` runtime arm substrate (r101.21).
#
# ON (shipping): the arm table, the `exp` command and the Debug rows are compiled
# in; every arm is RAM-only and starts at the value r101.20 already behaved with.
# OFF: the substrate, its strings and its command are compiled out entirely --
# this is the identity-control build in the r101.21 checklist, and with it OFF
# exparms::completionDrive() is a constexpr `true`, so the paint path is the
# pre-batch instruction stream.
# ---------------------------------------------------------------------------
option(CP_EXP "EXP1: compile the `exp` runtime arm substrate (all arms default to today's behaviour)" ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(crosspoint_metalio)
