cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(reader_line_spacing)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(opds_feed_error)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(jpeg_hw_select)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
add_subdirectory(em_dash_line_breaking)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(book_metadata_cache)
add_subdirectory(home_button)
add_subdirectory(refresh_policy)
add_subdirectory(gray16_quantiser)
# r164 THUMB1: the 2 bpp cover-thumbnail quantiser (no collapse, level->level, flat tone).
add_subdirectory(thumb_greys)
add_subdirectory(screen_geometry)
add_subdirectory(button_press_navigation)
# r101.21 EXP1: every arm defaults to r101.20 behaviour (the batch's failure #1).
add_subdirectory(exp_arms)
# r101.21b FLP1: the frontlight panel's repaint. Pure table + the REAL
# src/display/RefreshPolicy.h, so "adjust = one FAST, exit = one from-white FULL"
# is proven on the host without a panel.
add_subdirectory(frontlight_panel)
# r111 EOB1: the end-of-book panel's layout and repaint table. Pure header + the
# REAL src/display/RefreshPolicy.h, plus a call-site check on ReaderActivity.
add_subdirectory(eob_panel)

# BATCH 0 (GUARD) - see docs/planning/BATCH0_GUARD.md. One command runs them all:
#   ctest --test-dir build-b0-tests -L batch0 --output-on-failure
# The two known-red paint-policy contracts (they need a src/ change, out of scope
# for Batch 0) are named KnownRed_*; a green gate is `-L batch0 -E KnownRed`.
add_subdirectory(aud_gate)
add_subdirectory(policy_paint)

# BATCH 0 (GUARD) - static invariant: the standby resume must restore the P0.5
# CAM_SCR rail BEFORE it issues the FT6336U wake. RED on purpose (TCH3, parked);
# registered in test/EXPECTED_RED.txt.
add_subdirectory(standby_resume_order)

# r101.21 PWR4: the standby auto-off path must mount the card before composing the frame,
# and its journal line must name the frame that was painted. Structural (source-landmark)
# like standby_resume_order; GREEN from the commit that lands the fix.
add_subdirectory(poweroff_autooff)

# r111 PWR9: the power-off dialog frame's own composition, painted on a 1-bit surface on the
# host. RED before the fix (the word's ink equalled the field it sat on: border only).
add_subdirectory(poweroff_text_frame)

# r163 B5: the power-key gesture classifier's own boundaries, on the host. Click /
# multi-click / hold edges, the settle window, and the press inherited across a
# soft restart (which must be discarded, never classified as a click).
add_subdirectory(power_gesture_classifier)

# r163 E3: the charge truth table with its deadband, plus the one consumer question that
# is not arithmetic -- which of the two flags (raw vs displayed) each visible site uses.
add_subdirectory(battery_gauge_state)

# r163 E4: the charger control loop (EN_TERM gating, recharge kick, UVLO) as arithmetic, plus
# the wiring as text. No pack is needed and no charger register is touched by either.
add_subdirectory(charger_policy)

# BATT5: the licence for the battery icon's region patch -- "the canvas IS the frame on
# the glass" -- and the gate that refuses it. Pure arithmetic on lib/GfxRenderer/GlassEpoch.h,
# so the pin-then-patch path cannot be licensed by a stale framebuffer.
add_subdirectory(batt_icon_glass_epoch)

add_subdirectory(inflate_stream)
add_subdirectory(inflate_reader)
add_subdirectory(zip_file)
add_subdirectory(fs_helpers)
add_subdirectory(http_header)
add_subdirectory(txt_to_html)

add_subdirectory(absolute_grayscale)

add_subdirectory(gif_support)

add_subdirectory(sd_card_font)
add_subdirectory(clipping_preview)
add_subdirectory(glyph_bitmap)

add_subdirectory(clipping_store)
add_subdirectory(reader_session)
# r163 3.3: the reading statistics arithmetic (rate threshold, saturation, formatting).
add_subdirectory(reading_stats)
add_subdirectory(page_repeat)
add_subdirectory(plugin_manifest_classification)
add_subdirectory(plugin_picker_layout)
add_subdirectory(protected_paths)
# r163 A13: the live page record. Counts the HalFile::read calls a page load costs on the card,
# then proves a repaint of the same page costs none and draws the same page.
add_subdirectory(page_record_cache)
# r163 A2: the glyph lookahead's window, its per-font depth and the idle discipline that keeps it
# out of a paint and out of a build. Structural over src/ (the reader has no host seam).
add_subdirectory(prewarm_lookahead)
# r163 C3: the reader -> UI handoff contract as types (session generation, fixed completion slots,
# a Try-only display guard), plus the two rules over the reader sources that types cannot express.
add_subdirectory(reader_handoff)
# r163 C2: the bounded job queue and the domain's submit / idle-exit decisions. The task itself is
# not host-testable; the state machine that decides what it does is.
add_subdirectory(reader_jobs)
# r163 C1: cross-chapter preparation. The layout cannot run on a host; the decisions can -- the
# gate set, the byte/page budgets, the invalidation rule and the adoption rule -- so they are a
# pure header and the wiring is checked against the reader source.
add_subdirectory(chapter_prefetch)
# r163 C4: coalesced progress saves. The pending-snapshot store is pure, so the merge-to-latest,
# the reopen snapshot and the stale-completion rules run on the host.
add_subdirectory(progress_saves)
# BACKUP1: the full-profile envelope/restore contract. Pure module, so the
# rejection matrix (bad sha, schema from the future, invalid enum, unknown font,
# truncated JSON) runs on the host and the gate holds it green.
add_subdirectory(backup_profile)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME TrustedTimeTimezone COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_trusted_time.py)
add_test(NAME RemoteButtonInputIntegration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_remote_button_input.py)
add_test(NAME confirmation_translations COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_confirmation_translations.py)
add_test(NAME BleLearning COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ble_learning.py)
add_subdirectory(ble_link_note_title)
add_subdirectory(ble_chapter_build_gate)
add_subdirectory(ble_remote_turn_gate)

add_subdirectory(${REPO_ROOT}/lib/BlePageTurner/test BlePageTurnerTests)

add_test(NAME selection_buttons COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_selection_buttons.py)
add_test(NAME list_viewport COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_list_viewport.py)
add_test(NAME clipping_highlight_planes COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_clipping_highlight_planes.py)
add_test(NAME clipping_selection_text COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_clipping_selection_text.py)

add_test(NAME recursive_dictionary COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_recursive_dictionary.py)

# r165.5: the r163 Back-key contract (TAP = one level, HOLD = Home) and the r165.3 reader-origin fix.
# It compiles two real production bodies plus ActivityManager's reader-return trio against stubs and
# asserts the Library -> book -> sync -> book -> Back destination -- the operator's own report. It
# lived outside ctest, so when r165.4 changed uisounds::onEvent's signature the harness stopped
# compiling and NOBODY NOTICED for a whole round. Name is stable (the gate register and the docs
# refer to it); it fails by name, printing the case that broke and keeping the generated source in
# _scratch/back_key_contract/.
add_test(NAME BackKeyContract
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_back_key_contract.py)
set_tests_properties(BackKeyContract PROPERTIES LABELS "back_key;r165")

# r165.5: the last Python harness that existed but was never registered, so `ctest` ran the suite
# without it -- the same blind spot that let BackKeyContract rot for a round. (test_patch_animatedgif
# was already registered by gif_support/CMakeLists.txt.) It is green on this tree; if it goes red it
# must be fixed, not registered.
add_test(NAME PowerSleepGuardContract
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_power_sleep_guard_contract.py)

# BATCH 0 (GUARD): the unbounded-take-in-a-loop rule, as a static check.
add_test(NAME PortMaxLoopGuard
         COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/tools/portmax_guard.py)
set_tests_properties(PortMaxLoopGuard PROPERTIES LABELS batch0)

# BATCH 0 (GUARD): the gate runner's own classification fixtures. Pure Python --
# registered names that were never built must read REGISTER-UNRESOLVABLE, not
# REGISTER-STALE, so the gate cannot mistake "never built" for "passed".
add_test(NAME Batch0RunnerClassification
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_batch0_runner_classification.py)
set_tests_properties(Batch0RunnerClassification PROPERTIES LABELS batch0)

# BATCH 0 (GUARD): the shared-UI-theme slot rule.
add_test(NAME SlotGuard
         COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/tools/slot_guard.py)
set_tests_properties(SlotGuard PROPERTIES LABELS batch0)


# SIMD1 (TCH5a batch): the s8 dot-product image2d_t descriptor and arp4 eligibility,
# checked against the vendored esp-dsp ANSI kernel on the host. Appended at the end so
# it cannot disturb the existing registrations.
add_subdirectory(dotprod_layout)

# E5 (HARDWARE_POWER_PLAN §5.4): the touch probe ladder's decision half — attempt
# count, success latch, when the rail+reset recovery is due. Pure header, no
# hardware: the hardware verbs live in InputManager and are exercised on glass.
add_subdirectory(touch_probe_ladder)

# r106 B0: the park-sink BUILD DEFAULTS (usb ships OFF). Compiles the real
# src/platform/ParkSinks.cpp; the hardware legs fold out on the host.
add_subdirectory(park_sinks)

# r165 AUDIO1: the UI-sound event map and the drop-if-busy policy. Compiles the
# REAL src/audio/UiSoundPolicy.h -- the header the firmware's worker includes.
add_subdirectory(ui_sounds)

# r106 B1: the TCA port read-modify-write bit arithmetic. Compiles the same header the
# board layer (a C translation unit) includes, so the bytes asserted here are the bytes
# `trim cam cut` writes on glass.
add_subdirectory(tca_rmw)

# r110 WRITEGATE: halstorage::isWriteOflag -- the predicate that decides whether an open() can
# create/truncate/write on a read-only (unmarked) volume. Two executables, one source, so the
# verdicts are asserted under both SdFat flag encodings (USE_FCNTL_H 1 on ESP32, 0 elsewhere).
add_subdirectory(storage_write_gate)

# r163 E6 / 3.5: the declarative power NEED table (hard blocks automatic standby, soft
# is inert), its wiring in main.cpp / UsbMscActivity / the debug shell, and the
# charge-aware idle policy expressed as a need.
add_subdirectory(power_need)

# r163 D3: grey ADMISSION by content role -- the table itself (header-only) and the
# structural checks that keep it an admission rule and not a lane change.
add_subdirectory(grey_role)

# SET3 (r164): settings rows with a discrete set of options open an option list.
# The decision table, the dynamic font row's index <-> stored mapping, the real
# ListNav scroll behaviour across a push/pop, and the wiring as text.
add_subdirectory(settings_selector)

# PPA1 (r165): the PPA SRM selection decision. "The hardware path is identical"
# cannot be tested without the hardware; the DECISION can, and it is the thing
# that decides whether the render path ever leaves the CPU. 23 real buffers:
# every surface this tree owns is packed sub-byte and unaddressable, and each of
# the six refusal reasons is reachable.
add_subdirectory(ppa_srm_plan)

# IMU1 (r165): accelerometer tilt page turn. The whole decision chain is a pure
# function, so the traces that matter -- steady, sway, knock, pick-up, tilt held
# while touching, flip follow-through -- are replayed sample by sample.
add_subdirectory(imu_flick)

# CAD2 (r165): the two cadence pickers show 1/5/10/15/30/60/Never without moving a
# stored value. The position <-> slot table is pure, so all seven round trips and
# "Never is the last row, and it still stores 5" run on the host; the wiring test
# holds both surfaces to that one table.
add_subdirectory(cadence_order)
