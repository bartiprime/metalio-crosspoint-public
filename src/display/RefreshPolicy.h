#pragma once

// ---------------------------------------------------------------------------
// CP_RICKY_REFRESH_POLICY — the RickyOS refresh policy, behind one switch.
//
// Source: docs/engineering/RE_RICKYOS_PIPELINE.md (24-row table, rows 5, 7, 8,
// 9, 12, 23) and docs/engineering/REFRESH_POLICY_RICKYOS.md (what was adopted,
// what was not, and why). That document is the authority for every claim here;
// this header only encodes it.
//
// Split of responsibility, and it is deliberate:
//   - The REQUEST PRODUCER stays ours. `GfxRenderer::changedRatioPercent()` (the
//     measured 5 % AUTO ratio) and the one-shot `promoteNextRefresh()` remain the
//     only things that decide what a paint asks for. Pipeline row 21 is KEEP-OURS:
//     RickyOS never measures how much of the frame changed, and flattening our
//     measurement to their call-site + armed-bit scheme would be a capability
//     loss.
//   - The POLICY LAYER around it is theirs: clean-state precedence (row 5),
//     a latch of the mode actually applied (row 7), no HALF after FULL (row 8),
//     and a completion drive after a fast one (row 9).
//
// Every switch here is compile-time. Flipping all of them to 0 must reproduce
// r80 exactly, byte for byte, and both configurations are built and checked.
// ---------------------------------------------------------------------------

#ifndef CP_RICKY_REFRESH_POLICY
#define CP_RICKY_REFRESH_POLICY 1
#endif

// Row 9 sub-switch, independent on purpose: the completion drive is the one
// change here that costs a second panel flash per fast transition. If that
// double flash is intolerable on glass, this alone can be turned off while the
// rest of the policy stays in.
#ifndef CP_RICKY_COMPLETION_DRIVE
#define CP_RICKY_COMPLETION_DRIVE 1
#endif

// CP_VENDOR_LUT_ONLY — fast paints read the vendor waveform table itself.
// See the "Fast-paint LUT source" section of REFRESH_POLICY_RICKYOS.md: the
// vendor DU table (my_du_25_0_data) is what the panel vendor validated, while
// the boot-time binary projection in waveforms/my_waveform.c is ours.
#ifndef CP_VENDOR_LUT_ONLY
#define CP_VENDOR_LUT_ONLY 1
#endif

// CP_RICKY_TURN_MODE — a PAGE TURN requests the GL16 family instead of DU.
// RE_RICKYOS_MODE_MAP.md (pass 5) reads RickyOS's page turn as requesting their
// mode 6, a GL16-family entry with 48 phase words, set at 0x4019193e. We have no
// mode-6 equivalent (we have no GC16_FAST/GL16_FAST entry at all), so this
// approximates it with our GL16 — their key 5. A NEAR-MIRROR, NOT AN EXACT ONE.
// Everything that is not a page turn keeps the request it already made.
#ifndef CP_RICKY_TURN_MODE
#define CP_RICKY_TURN_MODE 1
#endif

// CP_READER_AA_LANE — r161, DEFAULT 1. r162: DEFAULT 0 and the reader arm no
// longer reads it (see ActivityManager.cpp's per-paint glyph arm).
//
// WHY IT IS OFF AGAIN. r161 made a page turn request FAST so the FRAME could pick
// the lane from `useGray8`. On this 4 bpp canvas a covered glyph keeps its 2-bit
// level, so the frame always carries grey and FAST therefore ALWAYS took the grey
// wipe composite (my_waveform_wipe_gl(3) -> 38 phases, full panel), whose first
// phase erases every pixel to paper: the page-turn white flash the operator
// reported. It also drove the CP_UI_GREY_CLEAN_EVERY counter, turning every 5th
// turn into a from-white clean. Pre-r161 asked for GL16, which the panel runs as
// epd_hl_update_screen(MODE_GL16) — a diff drive with no wipe prefix — and that
// is what ships.
//
// THE SETTING IS NOT THIS SWITCH. `SETTINGS.textAntiAliasing` now drives the GLYPH
// BLITTER only (`renderer.setTextBinary`, applied per paint in ActivityManager):
// ON = coverage glyphs, OFF = thresholded ones, on the SAME GL16 request. The
// dual-plane overlay path is untouched either way: `grayscaleCapabilities(Overlay)`
// is never claimed, so `needsAnyGrayscale` stays false and the reader never calls
// displayGray()/copyGrayscale*Buffers() — that path is the black-flip.
//
// WHAT `INERT` MEANT. `SETTINGS.textAntiAliasing` was also read at
// EpubReaderActivity.cpp and ANDed with that capability, which is the overlay
// gate and not the AA mechanism on this panel; that gate must keep answering
// "unsupported". It does not gate the blitter.
//
// At 0 (the shipped value) the reader is r160 byte for byte on the request side:
// page turns request GL16. At 1 the r161 A/B behaviour comes back.
#ifndef CP_READER_AA_LANE
#define CP_READER_AA_LANE 0
#endif

// ---------------------------------------------------------------------------
// CP_ICON_PARTIAL_REPAINT (default 1, ON as of r98)
//
// The battery charging glyph repaints ITSELF the moment CX25601N REG 0x1E flips
// (HalPowerManager::isCharging()), instead of waiting for the operator to
// navigate away to a page without the icon and come back. Implementation:
// src/display/BatteryIconRegion.cpp.
//
// It is built on the machinery that already exists, not on new waveform logic:
//   - DETECTION is an EDGE on isCharging() (main.cpp loop, once per iteration
//     against a cached bool refreshed at most once per BATTERY_POLL_MS). A steady
//     state therefore drives ZERO repaints and the 1.5 s poll is never fought.
//   - The REGION is the icon's own rectangle, recorded by
//     BaseTheme::drawBatteryOutline() -- the one primitive every theme funnels the
//     glyph through -- and invalidated at the top of every activity paint, so a
//     screen that does not draw the icon (a menu, a full-screen image, the sleep
//     chip) simply has no rect to repaint.
//   - The DRIVE is HalDisplay::FAST_REFRESH (DU) and nothing else. This tree has
//     no sub-area drive: every caller runs epd_full_screen() with a row/column
//     mask taken from the framebuffer-vs-glass diff (highlevel.c ->
//     render.c's `[mask]` line). Changing only the icon's box means the mask IS
//     the icon's box, so the smallest region this renderer supports is exactly
//     what an unchanged-everything-else repaint produces: no from-white clean, no
//     whole-panel GC16, no second completion flash over a frame that did not move.
//   - SKIPPED, never forced, when a one-shot (promotion / forced clean) is armed,
//     when the entry gate holds the frame's first drive, when a paint is in
//     flight, or when the icon is not on the visible screen. The skip is free:
//     every paint that draws the icon re-reads isCharging() live, so the deferred
//     update lands on the next frame that shows the icon.
//
// GHOSTING: two small DU drives per plug/unplug at most, confined to a 16x12-ish
// outline box that is repainted identically, and the periodic GC16 cadence that
// already exists (every N pages / the HALF cadence) clears the residue. The
// alternative -- routing this through GL16 (~1 s per toggle) -- buys nothing for
// an icon that changes twice a session.
//
// 0 reproduces r97.2 exactly at this call site: no rect is recorded, nothing is
// polled, and the legacy whole-activity repaint on the USB GPIO edge is the only
// producer (unchanged). See idf/CMakeLists.txt for the -D path.
// ---------------------------------------------------------------------------
#ifndef CP_ICON_PARTIAL_REPAINT
#define CP_ICON_PARTIAL_REPAINT 1
#endif

// ---------------------------------------------------------------------------
// CP_ICON_SAFE_PATCH (default 1) — the REPLACEMENT for CP_ICON_PARTIAL_REPAINT.
//
// Same goal (the charging glyph moves the instant isCharging() flips, on every
// screen including the reader, without a page re-render) and the same drive (the
// icon's own rectangle, diff-masked, pinned to DU). What differs is the LICENCE.
//
//   the old path: licensed by a RECORDED RECT      -> an inference
//   this path:    licensed by CANVAS == GLASS      -> a positive fact
//
// The canvas-vs-glass equality is lib/GfxRenderer/GlassEpoch.h: `composed` is
// bumped by every mutation of the framebuffer, `glass` is assigned from it only
// where the code accepts a drive that landed. When they differ the canvas holds
// a frame the panel has never shown (a paint the entry gate held, a paint the
// activity declined, a failed drive, a restored BW canvas after a grey pass) and
// a region patch is REFUSED -- the whole-activity DU repaint takes over, which is
// r164's behaviour byte for byte.
//
// THE BLANK FRAME IS IMPOSSIBLE, structurally, from three independent terms:
//   1. the patch refuses unless `composed == glass` (GlassEpoch.h: equality is
//      only ever established by accepting a drive, never synthesised);
//   2. the patch refuses unless the recorded rect was recorded AT that epoch, so
//      the rect belongs to the frame the glass holds;
//   3. the drive is pinned to the diff-masked DU with forceBinaryDuOnce() +
//      suppressCompletionDriveOnce() -- the FLP2 pair 58b4d2cc introduced -- so
//      the vendor shim cannot take the `my_waveform_wipe_gl(3)` lane (the 8-phase
//      erase prefix that turns "diff mask" into "whole panel white, then only the
//      mask repainted" = the corner-only blank screen) and there is no row-9
//      whole-panel follow-up.
// Points 1 and 2 are checked again INSIDE the RenderLock, immediately before the
// pixels are touched, so a paint that started while the edge was being serviced
// cannot invalidate the decision.
//
// THIS SWITCH DISABLES THE OLD PATH BY CONSTRUCTION. When it is 1,
// CP_ICON_PARTIAL_REPAINT is forced to 0 below, so `-DCP_ICON_PARTIAL_REPAINT=ON`
// cannot resurrect the retired self-repaint alongside it. 0 restores r164 exactly:
// main.cpp's `#else` branch (whole-activity DU repaint, reader excluded).
//
// Related: CP_ICON_RECT_RECORD is the rect bookkeeping BOTH paths need. It is a
// derived value, not a user switch.
// ---------------------------------------------------------------------------
#ifndef CP_ICON_SAFE_PATCH
#define CP_ICON_SAFE_PATCH 1
#endif

// ---------------------------------------------------------------------------
// CP_BATT_PERCENT_PATCH (default 1) — BATT6: the percentage NUMBER repaints when
// it changes, not when something else happens to repaint the screen.
//
// The glyph above answers ONE edge (charging). Nothing answered "the displayed SOC
// moved", so the digits froze at whatever the last paint saw -- on the glass: 100 %
// after a 7.5 h standby with the pack at ~90 %, refreshed by leaving the screen and
// coming back. The same freeze made BatteryStats record the pre-sleep value as
// current.
//
// Implementation: src/display/BatteryPercentPatch.cpp. It records the number's own
// box at every site that draws it through an app-side primitive
// (BaseTheme::drawBatteryLeft, AndroTheme's status band and bar, PickyOSTheme's
// band) and repaints exactly that box, in place, in ONE diff-masked DU drive, when
// the value the DROVE paint left on the glass differs from the gauge's current
// value. The glyph is erased and redrawn in the same drive when its box belongs to
// the same frame, because the fill level is the same percentage -- otherwise a
// 100 % bar would sit beside "90 %".
//
// The FreeInkUI header band (Home, library, settings, catalog) draws its number
// inside the SDK component, so there is no app-side box there: it records the VALUE
// it painted and a change costs ONE whole-activity repaint, which main.cpp pins to a
// diff-masked DU -- the panel still drives only the pixels that differ.
//
// The licence is the SAME one the glyph patch uses (lib/GfxRenderer/GlassEpoch.h:
// canvas == glass, box recorded at that epoch, no one-shot armed -- re-checked
// inside the RenderLock). No new mechanism, no new waveform, no clean:
//   * a steady percentage produces ZERO drives (the comparison is an edge against
//     the recorded value, never a timer);
//   * a change that cannot be serviced in place (a screen whose box was never
//     recorded) falls back to the whole-activity repaint main.cpp already does for
//     the glyph, pinned to DU, ONCE per value -- never once per loop pass;
//   * a screen that draws no percentage (full-screen image, sleep chip, the number
//     hidden in Settings) records no box and costs nothing.
//
// 0 compiles the producer out: no box is recorded, nothing is polled, and the
// behaviour is r165's (the digits move only when something else paints).
// ---------------------------------------------------------------------------
#ifndef CP_BATT_PERCENT_PATCH
#define CP_BATT_PERCENT_PATCH 1
#endif

#if CP_ICON_SAFE_PATCH && CP_ICON_PARTIAL_REPAINT
// The retired path and its replacement are mutually exclusive: two producers on
// one edge would spend two drives on one plug, and the old one is the one that
// drove a framebuffer which was not the frame on the glass.
#undef CP_ICON_PARTIAL_REPAINT
#define CP_ICON_PARTIAL_REPAINT 0
#endif

// The rect record lives in GfxRenderer::BatteryIconRect (see the header): written by
// BaseTheme::drawBatteryOutline(), cleared at the top of every activity paint and
// committed by ActivityManager at the end of it. It is not a separate switch -- both
// producers share it, and it is only ever *used* together with the epochs above.

// ---------------------------------------------------------------------------
// CP_EPD_POWERDOWN_AFTER_DRIVE (default 1, ON as of r84)
//
// H1 of docs/engineering/EPD_STATIC_IDLE_ARTIFACT.md — CONFIRMED statically:
// "we never power the panel down; the rails stay energised with the source
// outputs high-Z (epd_set_mode(0), render_lcd.c:234), so the pixel charge is
// trapped with the field still live and nothing balances it." That is the
// mechanism behind the dark gradient creeping into the bottom-left corner of a
// static screen. Evidence line: EPD_STATIC_IDLE_ARTIFACT.md §2 — "the only
// power-down on this path" (metalio_vendor_panel.cpp:471-474) is conditional on
// turnOffScreen, which defaults false and whose only producer
// (SettingsList.cpp:73) is emitted only on boards WITHOUT touch, so on this
// board the mitigation is compiled out.
//
// RickyOS brackets EVERY drive with a power-up and a power-down (their log line
// is "on= draw= off=", RE_RICKYOS_PIPELINE.md row 24), and brings the rails down
// BEFORE holding the bus at park (RE_RICKYOS_SLEEP_WAKE.md). We adopt the first
// half of that here: down after every drive.
//
// With this ON, the paint funnel (HalDisplay::displayBuffer / refreshDisplay /
// displayBufferAsync) issues the existing datasheet-8.4 power-down path
// (metalio_vendor_panel_off -> epd_poweroff, epd_board_metalio_eink4_plus.c:889)
// once the drive has completed, and waits the section-8.4 tail (PWRUP/VCOM down
// -> WAKEUP down -> >=100 ms) before returning. The next paint re-powers through
// metalio_vendor_panel_show's PWR_GOOD check (metalio_vendor_panel.cpp:366-378)
// and its g_powered gate (:372-375).
//
// KNOWN COST, measured statically from the delay constants in that path — the
// full arithmetic is in EPD_STATIC_IDLE_ARTIFACT.md §7.3, and it is not small:
//   power-down + tail  ~122 ms  (12 us XOE gap + 3 expander writes + 1 ms tick
//                               + our delay(120))
//   next power-up      ~135 ms  (50 + 50 ms settles, ~5 ms I2C, PWR_GOOD poll
//                               typically 10-60 ms, PG poll 1-5 ms; worst case
//                               ~1075 ms if the part takes all 3 retries)
//   => ~257 ms per paint, and a page turn drives TWICE (FAST + the row-9
//      completion drive, CP_RICKY_COMPLETION_DRIVE=1) => 2 full off/on cycles,
//      ~514 ms from keypress to the next frame on the glass.
// AS OF r85 this is the r84 arithmetic. CP_TPS_FAST_SEQUENCE=1 (below) removes
// both 50 ms settles and the per-drive 120 ms tail, leaving ~1 ms down plus the
// polled power-good rise: ~257 ms becomes single-digit-to-tens of ms per paint.
// Setting CP_TPS_FAST_SEQUENCE=0 reproduces the numbers above exactly.
// This is a real interactive-latency regression (~0.25 s per UI update, ~0.5 s
// per page turn) accepted on the hypothesis that an unpowered panel cannot
// drift. If the glass says the gradient survives it, turn this back off and
// take the ART2 candidate instead (power down when the UI goes IDLE, not after
// every drive).
// ---------------------------------------------------------------------------
#ifndef CP_EPD_POWERDOWN_AFTER_DRIVE
#define CP_EPD_POWERDOWN_AFTER_DRIVE 1
#endif

// ---------------------------------------------------------------------------
// CP_TPS_FAST_SEQUENCE (default 1, ON as of r85)
//
// Makes the per-drive TPS65185 cycle cost what RickyOS's costs. Source:
// docs/engineering/RE_RICKYOS_POWER_TIMING.md — their refresh (0x40190608)
// powers the rails up at entry and down at exit on ALL seven exits, from all
// three drive sites, and that cycle is ~1.01 ms down plus a POLLED power-good
// rise, because:
//   §2.1/§3  their power-up has NO fixed settle constant at all: control bit4
//            goes up, UPSEQ is written immediately, then PG is polled
//            (60 x 1 ms, then 200 x 1 ms with 3 attempts 100 ms apart). Where
//            we spend 50 ms after PWRUP-low and 50 ms after PWRUP-high, they
//            spend nothing and let the poll be the wait.
//   §2.2/§2.4 their per-drive down is control bit4 down -> 12 us -> word 0 ->
//            1 tick -> WAKEUP down, which is exactly our
//            epd_board_poweroff(); their DWNSEQ0/1 are never written either, so
//            the part's default down timing applies on both sides.
//   §2.3    the 50 ms (conditional) + 120 ms rail-collapsing wait lives ONLY in
//            their park function (0x40192456, one caller: the standby park).
//
// So the 120 ms section-8.4 tail we hold after every drive is ours alone. It
// exists for a real reason — §8.4 says >=100 ms between rails-down and the next
// power-up, and with DWNSEQ unwritten the part's own down sequence (default
// timing) is what that gap protects — but it is unnecessary for a transient
// per-drive cycle that re-powers within milliseconds, and it is the single
// largest term in r84's ~122 ms per-drive down.
//
// With this 1:
//   - epd_board_poweron_once() drops both vTaskDelay(50) settles and relies on
//     the existing PWR_GOOD poll (200 x 1 ms, 3 attempts) as their gate. A NAK
//     on the UPSEQ/REVID writes of a cold part is warned about and continued,
//     which is RickyOS's own contract (its "UPSEQ write" line is a WARNING).
//   - cp_powerdown_after_drive() (lib/hal/HalDisplay.cpp) holds the 120 ms tail
//     ONLY where the rails are being left down for a while: the explicit
//     turnOffScreen path and park/sleep (tps_enter_sleep() already pays its own
//     50 + 120, epd_board_metalio_eink4_plus.c:343-361). The per-drive cycle
//     skips it.
// With this 0: r84 timing exactly — 50 + 50 ms settles on power-up and the
// 120 ms tail after EVERY drive. One flag restores the previous release.
//
// What a wrong call here looks like on glass: NOT a reboot and NOT a blank
// screen. Skipping the tail risks the rails not being fully collapsed before
// the next power-up, which shows up as ghosting or a stuck scan line after a
// burst of rapid page turns. Skipping the settles risks a NAK'd UPSEQ on a cold
// part, i.e. a slower/softer boost ramp, still non-fatal. This is one unit's
// evidence (RE_RICKYOS_POWER_TIMING.md §5, "one data point, on one unit"), so
// the revert path above is part of the ship.
// ---------------------------------------------------------------------------
#ifndef CP_TPS_FAST_SEQUENCE
#define CP_TPS_FAST_SEQUENCE 1
#endif

// ---------------------------------------------------------------------------
// r82 — the SLEEP / WAKE pair. Sources: docs/engineering/RE_RICKYOS_SLEEP_WAKE.md
// (this pass) and RE_RICKYOS_TRANSITIONS.md rows 38/39.
//
// All three default to 0. With all three OFF the firmware must behave exactly
// as r81 did: the sleep screen is still FULL on this board (see below), the bus
// is driven low but NOT latched, and standby resume still paints once with a
// promoted FULL and no throwaway drive. The operator enables these on glass.
// ---------------------------------------------------------------------------

// CP_RICKY_SLEEP_PAINT — the sleep screen is always a from-white GC16 (mode 2),
// i.e. kSleepClean is FULL_REFRESH on every board instead of HALF where a board
// lacks a from-white path. RickyOS's sleep paint reaches mode 2 by the
// clean-panel rule (row 5, 0x4019060a-0x4019062a), not by a call-site constant,
// so their sleep screen is a whole-panel GC16 wherever it lands.
//
// On THIS board the switch is behaviour-neutral: `kSleepClean` is already
// FULL_REFRESH for Metalio (SleepActivity.cpp:39-43) and has been since the
// striping report. It is kept as a switch because that #if is board shape, not
// policy, and because the operator's A/B needs a value that can be flipped
// without editing the activity.
#ifndef CP_RICKY_SLEEP_PAINT
#define CP_RICKY_SLEEP_PAINT 0
#endif

// CP_EPD_BUS_HOLD — hold the 16 EPD bus pins through standby, not just drive
// them low. The pin list, the order and the "no wake source" proof are in
// RE_RICKYOS_SLEEP_WAKE.md §2. This is the one change in this release that can
// brick a wake: gpio_hold_en on the wake line (GPIO2, the expander INT — the
// Plus has no power-button GPIO at all) would leave the device unable to leave
// standby, and recovery is USB flashing via the BOOT strap, i.e. not remote.
// The list is pins of the EPD connector only; GPIO2 is not in it and must never
// be added to it.
#ifndef CP_EPD_BUS_HOLD
#define CP_EPD_BUS_HOLD 0
#endif

// CP_CUT_PA_RAIL_ON_STANDBY — de-assert TCA P1.4 (BT_PA_PWR, the 4G + BT-audio +
// NS4150 amp shared rail) for the duration of a standby, and re-assert it on wake.
// Default ON from r99.1: the measurement is closed (round 3, 3 Nov 2026: 10.38 mA
// trimmed vs 17.53 mA untrimmed, -40.7 %, standby 4.75 -> 8.01 days, 7/7 parks armed,
// zero resets). This #ifndef is only the fallback for a build that does not define it;
// the CMake option CP_CUT_PA_RAIL_ON_STANDBY (idf/CMakeLists.txt) is what ships it,
// and the runtime override (`trim pa off`, the Debug arm row) still wins.
//
// P0.5 (CAM_SCR_EN) now has its own switch below (CP_CUT_CAM_RAIL_ON_STANDBY); the two
// rails are gated INDEPENDENTLY so either can be A/B'd on the device without a rebuild.
// The only recorded failure of a rail cut on this unit drove P0.5 + P1.2 + P1.4 low
// *together* (the `latch_low3` ladder, ensureBooted()'s warning) and it was never an
// isolation test -- the single-rail steps (CP_MASK ladder cases 24/25/26) are recorded
// as never run. What changed the decision for P0.5 is the vendor evidence: their own
// firmware for this exact board cuts P5 (CAM_SCR) then P14 (BT_PA_PWR) and nothing else
// (epd_board_metalio_eink4_plus.c:331-342 in the vendor tree), and restores P14 ->
// settle -> P5 (:344-362). It never touches P1.2 (TP_RST), and neither do we.
//
// Safe order (this is what the code below implements, see main.cpp park/resume):
//   entry: panel parked -> TPS in SLEEP -> EPD bus held low -> THEN P1.4 low, P0.5 low.
//   wake:  P1.4 high -> 80 ms rail settle -> P0.5 high -> EPD bus restore -> epd_poweron().
// Reversing it puts the BT module's inrush on the same 3V3 domain while the TPS
// boost is ramping, inside the 200 ms x 3 PWR_GOOD window
// (epd_board_metalio_eink4_plus.c:915-947): the gate can time out, no rails come
// up, and the panel stays blank until the next paint retries.
//
// SHIPPING VALUE IS 1, NOT 0. The 0 below is a FALLBACK, and nothing about it is what
// ships: idf/components/crosspoint_app/CMakeLists.txt declares
// `option(CP_CUT_PA_RAIL_ON_STANDBY ... ON)` and emits
// `target_compile_definitions(... CP_CUT_PA_RAIL_ON_STANDBY=1)`, which the compiler sees
// before this header, so the #ifndef never fires in a real build. The 0 is only what a
// build gets when it does not go through that option at all (a bare compilation of this
// header, or a target that never picks up the component's definitions). Do not read the
// value on the #define line as the shipping default; a previous version of this comment
// said "Default ON" over a 0, and that is the kind of contradiction that has cost us a
// session already.
#ifndef CP_CUT_PA_RAIL_ON_STANDBY
#define CP_CUT_PA_RAIL_ON_STANDBY 0  // fallback only: the shipping build defines this as 1
#endif

// CP_CUT_CAM_RAIL_ON_STANDBY — de-assert TCA P0.5 (CAM_SCR_EN, the camera/screen rail)
// for the duration of a standby, and re-assert it on wake. Independent of the P1.4 trim
// above: the two have separate runtime overrides, so `trim cam off` leaves the BT/PA cut
// in place and vice versa.
//
// WHY THIS EXISTS AT ALL, given the 25 Oct dark board: the vendor firmware for THIS board
// cuts exactly these two rails and nothing else on its way to standby
// (vendor epd_board_metalio_eink4_plus.c:331-342: P5 then P14; restore :344-362: P14 ->
// POWER_RAIL_SETTLE_MS -> recheck -> P5). Our bring-up incident drove P0.5 + P1.2 +
// P1.4 low *together*, which is a superset the vendor never asks for -- the difference is
// TP_RST (P1.2). P1.2 is therefore the one pin this project must never drive low, and
// neither trim does: pa_rail_set() owns P1.4 only, cam_rail_set() owns P0.5 only, and
// neither touches the touch reset that the FT6336U hibernation in HalGPIO owns.
//
// ORDER (see main.cpp metalioStandbySleep):
//   park:  P1.4 low, then P0.5 low -- after the panel, TPS and EPD bus are parked.
//   wake:  P1.4 high -> 80 ms -> P0.5 high -> (existing KWAKE_SETTLE_MS) -> first drive.
// Stated plainly because it does NOT match the vendor line-for-line: their cut is P5
// first, P14 last (:331-342). Ordering only has an electrical consequence on the way UP
// (two inrush loads must not land inside one PWR_GOOD window), and there we follow the
// vendor exactly -- P14 -> settle -> recheck -> P5. The park order is the operator's
// decision: the larger load (BT/PA) leaves first, the camera/screen rail is the last
// thing to drop. A rail going down draws no inrush, so either cut order is quiet.
// The 80 ms is the same figure epd_board_init() uses for a previously-cut P1.4, and it is
// the vendor's POWER_RAIL_SETTLE_MS between the two rails. Raising P0.5 before P1.4 has
// settled stacks the two inrush loads on 3V3 inside the TPS PWR_GOOD window.
//
// VALUE IS 0, AND THAT IS DELIBERATE. (Corrected 15 Nov 2026: this block used to claim
// "SHIPPING VALUE IS 1, NOT 0" and that CMakeLists declared the option ON. It does not —
// idf/components/crosspoint_app/CMakeLists.txt:510 declares it OFF, and the shipped r155
// image was compiled with CP_CUT_CAM_RAIL_ON_STANDBY=0. The two comments disagreed and the
// code agreed with the OFF one: src/main.cpp:2292 states CAM defaults OFF on purpose, armed
// per session with `trim cam on` / the Debug row, RAM-only, so a fatal cut costs one power
// cycle and never a reflash.)
#ifndef CP_CUT_CAM_RAIL_ON_STANDBY
#define CP_CUT_CAM_RAIL_ON_STANDBY 0  // shipping value: OFF; arm per session with `trim cam on`
#endif

// CP_SD_PARK_ON_STANDBY — unmount + park the card for the duration of a standby
// instead of leaving it mounted. Shipping ON from r99.1 for the same measured reason as
// the rail cut above; SD remount measured 68-69 ms inside the 250 ms settle budget.
// The cost it was never worth paying for an experiment (no journal writes while
// parked) is already accounted for: the journal's park line is written before the
// unmount and the resume line after the remount.
//
// There is no SD rail gate on this board, so HalStorage::prepareForDeepSleep()
// (unmount + float the SDMMC pads) is the only lever that exists. It costs one
// remount per standby -- not per slice: the card is parked at standby entry and
// restored on the resume path, before anything else touches storage.
//
// SHIPPING VALUE IS 1, NOT 0. Same mechanism as CP_CUT_PA_RAIL_ON_STANDBY:
// idf/components/crosspoint_app/CMakeLists.txt declares
// `option(CP_SD_PARK_ON_STANDBY ... ON)` and emits
// `target_compile_definitions(... CP_SD_PARK_ON_STANDBY=1)`, which wins over this
// header. The 0 below is only the fallback for a build that does not go through that
// option; it is not the shipping default and must not be read as one.
#ifndef CP_SD_PARK_ON_STANDBY
#define CP_SD_PARK_ON_STANDBY 0  // fallback only: the shipping build defines this as 1
#endif

// CP_RICKY_WAKE_PRIME — standby resume spends a throwaway FULL drive before the
// frame the user sees, exactly as our auto-off path already does
// (metalioHardPowerOff → wake -> settle -> throwaway FULL -> paint). RickyOS's
// wake paints once from white (row 13); ours paints once on warm rails after a
// 250 ms settle, which is the arrangement measured to band (W22). This switch
// makes resume use the arrangement that is measured clean.
//
// Cost: one extra whole-panel drive per wake (~0.8 s of waveform) on top of the
// settle. That is the price the auto-off path already pays.
#ifndef CP_RICKY_WAKE_PRIME
#define CP_RICKY_WAKE_PRIME 0
#endif

// Mode codes. These mirror HalDisplay::RefreshMode's ordinals exactly, so the
// renderer can pass static_cast<int>(mode) and the C driver side can use the
// same numbers. They are NOT EPDiy's EpdDrawMode values (pipeline row 1).
#define CP_RM_FULL 0  // from-white GC16 (48 phases, whole panel)
#define CP_RM_HALF 1  // GC16 over the diff area (48 phases, cleans)
#define CP_RM_FAST 2  // DU (20 phases, never cleans)
#define CP_RM_GL16 3  // GL16 (48 phases, drives the 15->15 tick)

// Latch values, mirroring RickyOS's *(this+87) as far as it is understood
// (pipeline rows 5 and 7). We collapse their "1 = fast applied" / "2 = full
// settle applied" onto: was the last thing on the glass laid down by a
// from-white FULL (2) or by anything cheaper (1).
//
// Why the collapse, stated because it is a deviation: their encoding writes 2
// after the row-9 completion drive, and row 8 then promotes the next fast
// request to GC16 — which writes 2 again. Taken literally that is a permanent
// latch and the fast path never runs again. Row 8's own wording is "do not
// follow a FULL with a HALF", so the term tracks a genuine from-white FULL
// only, and a row-8 promotion clears it after one paint. Exactly one
// half->full promotion per real full, which is what row 8 asks for.
#define CP_LATCH_UNKNOWN (-1)  // no trustworthy claim about the glass
#define CP_LATCH_CLEAN 0       // never painted since power-on / deinit / park
#define CP_LATCH_FAST 1        // last drive was a diff or a fast drive
#define CP_LATCH_FULL 2        // last drive was a from-white FULL

#ifdef __cplusplus
extern "C" {
#endif

// Producer stage: unchanged r80 precedence, kept as its own pure function so the
// composition "promotion beats the call site" is testable without a renderer.
// `auto_mode` is what the measured ratio resolved to (CP_RM_FAST below the
// threshold, CP_RM_HALF above it) — the caller computes it, this only orders it.
static inline int cp_refresh_resolve_request(int caller_mode, int auto_pending, int auto_mode,
                                             int promoted_pending, int promoted_mode) {
  if (auto_pending) return auto_mode;
  if (promoted_pending) return promoted_mode;
  return caller_mode;
}

// Policy stage: pure. No state, no HAL, no logging, no side effects.
//
//   request      mode the producer resolved to (see above)
//   clean        panel known-clean: never painted since power-on/deinit/park
//   last_applied latch value above; CP_LATCH_UNKNOWN when nothing is claimed
//   full_request the paint replaces the whole frame (a whole-frame GC16, never
//                a fast-band drive)
//
// Returns the mode that should actually be driven.
static inline int cp_refresh_resolve_mode(int request, int clean, int last_applied,
                                          int full_request) {
  // Rule 1 (row 5): a clean panel outranks both the armed promotion and the
  // ratio-derived request. An explicit FULL survives — it *is* the from-white
  // GC16, i.e. the strongest cleaning drive there is, and demoting it to a diff
  // would re-introduce the under-drive this rule exists to remove.
  const int gc16_or_stronger = (request == CP_RM_FULL) ? CP_RM_FULL : CP_RM_HALF;
  if (clean) return gc16_or_stronger;

  // Rule 2 (row 4, kept as-is): a whole-frame replacement is GC16, not a
  // fast-band drive. Our HALF is "GC16 over the diff", our FULL is "GC16 from
  // white"; both satisfy it, and the caller's explicit FULL is the stronger.
  if (full_request) return gc16_or_stronger;

  // Rule 3 (row 8): never follow a FULL with a fast-band drive. One paint is
  // promoted; the promotion itself writes CP_LATCH_FAST, so it happens once.
  if (last_applied == CP_LATCH_FULL && (request == CP_RM_FAST || request == CP_RM_HALF)) {
    return CP_RM_HALF;
  }
  return request;
}

// Panel parked / deinitialised / powered off: whatever is on the glass can no
// longer be inferred, so the latch drops back to clean. Implemented in
// lib/GfxRenderer/GfxRenderer.cpp and called from HalDisplay::deepSleep().
void cp_refresh_policy_note_park(void);

// A page turn's request (CP_RICKY_TURN_MODE). `fallback_request` is what the call
// site would have asked for without this port. Applied at the page-turn paint
// sites only (ReaderUtils::displayWithRefreshCycle and the reader's image-page
// base), never globally: a list scroll, a settings row or a keystroke must keep
// its cheap DU drive, because that is what RickyOS does too (their request value 1
// for every non-clean diff paint — RE_RICKYOS_MODE_MAP.md).
static inline int cp_refresh_page_turn_mode(int fallback_request) {
#if CP_RICKY_TURN_MODE
  (void)fallback_request;
  // Their mode 6 approximated by our GL16 (their key 5). No mode-6 entry is
  // synthesised: an UNKNOWN slot is not invented here, and no waveform table is
  // touched.
  return CP_RM_GL16;
#else
  return fallback_request;
#endif
}

#ifdef __cplusplus
}
#endif