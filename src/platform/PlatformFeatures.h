#pragma once

// ---------------------------------------------------------------------------
// Platform feature switches — the accelerators this SoC has and we do not use.
//
// Both default to 0. With both 0 the Debug page still shows the two rows, and
// they report "off"; nothing in the firmware calls into either service, and no
// ULP binary is linked. See docs/engineering/LPCORE_AND_PPA_FRAMEWORK.md.
//
// Why this header and not src/display/RefreshPolicy.h: RefreshPolicy.h is the
// panel-refresh policy (which waveform a paint asks for). These two switches are
// about a secondary CPU and a DMA pixel engine — neither of which any refresh
// decision depends on. Putting them beside the refresh policy would invite a
// future reader to assume the renderer consults them.
//
// What each switch means, exactly:
//   CP_LP_CORE — the Debug "LP core" row loads the ULP-LP-core binary into LP
//     RAM, starts it, samples its counter twice, reports the delta, and STOPS
//     it. Nothing else. It is NOT wired to sleep, to page idle, or to input, and
//     nothing starts the LP core at boot.
//   CP_PPA — the Debug "PPA self-test" row registers a PPA SRM client, runs one
//     scale/rotate on a known RGB565 buffer and compares the result against a
//     software reference. It is NOT wired to image rendering or to the EPD
//     frame buffers.
//
// CONFIG dependency: CP_LP_CORE additionally needs the ULP co-processor
// compiled in (CONFIG_ULP_COPROC_ENABLED=y + CONFIG_ULP_COPROC_TYPE_LP_CORE=y),
// because only then does IDF build and embed the ULP binary. Those four lines
// live in the TRACKED idf/sdkconfig.defaults since r87. Turning the switch on in
// a configuration without them is not an error: the service reports
// "not built" and names the missing symbols.
// ---------------------------------------------------------------------------

// r97 — OPT-IN, AND HEAD MEANS SHIPPING. All five accelerator/bench switches in this
// file (CP_LP_CORE, CP_PPA, CP_CPU_STATS, CP_LP_BENCH, CP_PPA_BENCH) default to 0, so a
// plain `idf.py -B ../build-r90c build` of this tree produces the SHIPPING image: no ULP
// binary reference, no ppa_* symbol, no bench command, no run-time stats reader.
//
// r87 and r96 reached their device-test images by hand-editing these lines to 1. That is
// the habit this round removes: a hand edit is invisible in a diff of the artifact and
// easy to leave behind, which is how "HEAD" stopped meaning "shipping".
//
// TO BUILD THE BENCH IMAGE, pass ONE CMake switch — never edit this file:
//     idf.py -B ../build-bench -DCP_BENCH=ON build
// CP_BENCH=ON defines all five to 1 (forwarded by
// idf/components/crosspoint_app/CMakeLists.txt). Nothing else differs between the two
// artifacts, which is what makes size and string comparisons between them meaningful.
#ifndef CP_LP_CORE
#define CP_LP_CORE 0
#endif

#ifndef CP_PPA
#define CP_PPA 0
#endif

// ---------------------------------------------------------------------------
// CP_LP_STANDBY — r99: let the LP core pre-filter the power key while the HP
// core is in standby, instead of the HP core waking on every GPIO2 edge.
//
// WHY. Standby today (r42, src/main.cpp metalioStandbyLoop) arms EXT1 ANY_LOW on
// GPIO2 plus a 60 s timer, so EVERY source on that wire-ORed line — charger
// PWR_GOOD, RTC alarm, IMU INT1, expander glitches — pays a full HP wake, a
// slice re-arm and (in the spurious case) an SD journal line. Measured standby is
// 10.43 mA average (5 % of 2000 mAh over 9.584 h). 576 timer wakes/night cannot
// explain that, so the cost is the light-sleep baseline plus those extra HP
// wakes. The LP core is the only architectural lever that removes the latter
// without touching the SD-mount or rail decisions (docs/LP_CORE_CAPABILITY.md).
//
// WHAT IT DOES, and only with this switch at 1:
//   * starts the sentry ULP program (idf/components/cp_lp_core/ulp/main_sentry.c)
//     armed on LP_TIMER(20 ms) | LP_IO(GPIO2) before the first slice, in
//     PRE-FILTER mode;
//   * arms `esp_sleep_enable_ulp_wakeup()` + the SAME 60 s timer heartbeat, and
//     does NOT arm EXT1 — that is the whole point, and the reason the press is
//     detected one level lower than the HP core;
//   * treats ESP_SLEEP_WAKEUP_ULP as a candidate press and runs the unchanged
//     metalioVerifyStandbyWake() on it, so a false positive still costs only a
//     spurious wake;
//   * stops the sentry and hands GPIO2 back on every exit from the loop.
//
// SAFETY, by construction:
//   * the 60 s heartbeat stays armed, so the device can never fail to wake;
//   * if the sentry will not start, the loop runs today's code verbatim
//     (EXT1 on GPIO2) — a one-line `sentryActive` check, not a second code path;
//   * any failure inside a slice also clears the sentry for the rest of standby.
// The cost of a MISSED press is therefore latency, not a hang: the press is
// picked up at the next 60 s heartbeat. That trade is the reason this ships OFF
// and why `lpsentry` exists to measure it before anyone turns it on.
//
// DEFAULT 0. One-line revert, and the shipping image contains no sentry code,
// no sentry blob and no ulp_lp_core_* symbol (r87 guard).
// TO BUILD A TEST IMAGE: idf.py -B ../build-lp -DCP_LP_STANDBY=ON build
// (forwarded by idf/components/crosspoint_app/CMakeLists.txt).
// ---------------------------------------------------------------------------
#ifndef CP_LP_STANDBY
#define CP_LP_STANDBY 0
#endif

// ---------------------------------------------------------------------------
// CP_USB_SHELL — an esp_console REPL on the EXISTING USB-Serial/JTAG console
// (the port the logs already go to). Phase 1 of
// docs/engineering/USB_DEBUG_CHANNEL.md (§3, option C). Not a second CDC and
// not the OTG composite: the board has ONE connector time-shared by GPIO36
// ANA_SW, so a composite cannot coexist with this console (design doc §1.2).
// Phase 2 (OTG composite: CDC0 console + CDC1 shell + MSC) is a separate round
// and reuses the same command table and the same mailbox.
//
// COMPILE-TIME switch = "is the capability in the image". Default 1 for this
// test round. One-line revert: change the 1 below to 0, or configure with
//     idf.py -B ../build-u -DCP_USB_SHELL=OFF build
// (idf/components/crosspoint_app/CMakeLists.txt forwards the option; with it
// OFF the define is CP_USB_SHELL=0 and every esp_console symbol is compiled
// out — src/debug/*.cpp is #if-guarded as a whole).
//
// WHETHER IT RUNS is a separate RUNTIME setting: Debug page -> "USB debug
// shell", persisted as CrossPointSettings::usbDebugShell (key "usbDebugShell",
// default 0 = off). A device that never opts in has no shell task and no
// stdin reader, only the log stream it always had.
// ---------------------------------------------------------------------------
#ifndef CP_USB_SHELL
#define CP_USB_SHELL 1
#endif

// ===========================================================================
// r96 BENCH SWITCHES — LP core + PPA measurement round.
// Plan and contract: docs/engineering/LPCORE_PPA_R96_PLAN.md §4.1.
//
// THREE INDEPENDENT SWITCHES, one per bench item, so each revert is one line:
//   CP_CPU_STATS  FreeRTOS run-time statistics + `cpu` / `pm` / `bench render`.
//                 The FreeRTOS trio itself lives in the TRACKED
//                 idf/sdkconfig.defaults (it cannot live behind a #define —
//                 it is a Kconfig option). This switch decides whether the
//                 firmware READS it and whether the three commands exist.
//   CP_LP_BENCH   a second ULP program (heartbeat + LP_IO_NUM_5/2 edge counters
//                 + optional HP wake) and the `lpbench` / `lpsleep` commands,
//                 plus the Debug rows "LP bench" and "LP sleep test".
//   CP_PPA_BENCH  the aligned/cache-safe ppa::Buffer wrapper, ppa::benchFullFrame()
//                 and the `ppabench` command, plus the Debug row "PPA bench".
//
// DEFAULTS. The SHIPPING value of all three is 0: with them at 0 no run-time
// stats reader, no bench ULP program reference, no aligned PPA allocation and
// no bench command exists in the image, and the r87 "zero accelerator symbols"
// guard still holds (0 ppa_*, 0 ulp_lp_core_*, no _binary_ulp_* symbol).
//
// THIS TREE IS THE SHIPPING IMAGE (r97): the three are 0 below, and so are
// CP_LP_CORE/CP_PPA above. The r96 bench artifact (M:/tmp/r96-final.bin) was built from
// a hand-edited copy of this file with all five at 1; from r97 the bench image is
//     idf.py -B ../build-bench -DCP_BENCH=ON build
// and nothing in the tree has to change for it, so a diff between the two binaries is a
// diff of these five switches alone.
// ===========================================================================
#ifndef CP_CPU_STATS
#define CP_CPU_STATS 0
#endif

#ifndef CP_LP_BENCH
#define CP_LP_BENCH 0
#endif

#ifndef CP_PPA_BENCH
#define CP_PPA_BENCH 0
#endif

// ---------------------------------------------------------------------------
// CP_IMG_CLEAN_ON_ARRIVAL (r96, D1) — a page that CONTAINS IMAGES arrives on
// clean glass.
//
// THE PROBLEM, measured from the tree rather than guessed: an image page used to
// resolve to `HALF_REFRESH` or the page-turn mode
// (src/activities/reader/EpubReaderActivity.cpp:2140-2143), never to a from-white
// drive, and the panel's own history is then visible under the picture. The
// operator's report — "image tone fix looks better now, but there's ghosting ...
// loading pictures carries the most ghosting if the screen is not cleaned" — is
// exactly that: the pixels are right, the glass underneath was dirty.
//
// WHAT IT DOES: when a page whose Page::hasImages() is true is shown for the
// FIRST time, arm the existing r93 one-shot (renderer.forceCleanNextRefresh(),
// consumed by the `forceClean` term in RefreshPolicy), so that paint is a single
// from-white GC16 and nothing else. It does NOT re-arm while the same page is on
// the glass, so the repeated repaints of one page (status bar, clippings, a
// menu over it) pay nothing.
//
// THE COST, stated: a from-white clean is ~815 ms on this panel
// (docs/engineering/REFRESH_MODES_PRIMER.md:80) and it lands on the FIRST paint of
// the image page — i.e. on the page turn that brings it in, which is already
// ~514 ms of waveform. Flipping fast through a picture book therefore pays ~+0.3 s
// per image page. That is the trade the operator asked for ("if it doesn't we need
// to add that"); set this to 0 to fall back to the previous HALF/DU behaviour.
//
// NOT the same thing as pagesUntilFullRefresh: that counter makes the page AFTER
// an image page clean. This makes the image page itself clean.
// ---------------------------------------------------------------------------
#ifndef CP_IMG_CLEAN_ON_ARRIVAL
#define CP_IMG_CLEAN_ON_ARRIVAL 1
#endif

// r96.1 — WHERE IN THE ARRIVAL THE CLEAN LANDS, decided deliberately.
//
// An image page can paint twice on arrival: renderContents() has an EARLIER branch
// (EpubReaderActivity.cpp:2129-2134) that drives the page with placeholder boxes on
// a FAST waveform while the images decode, and then the image branch below it draws
// the real thing. Two options:
//   (a) arm before the placeholder drive, so the PLACEHOLDER frame is the clean and
//       the real image lands as a normal update over clean glass;
//   (b) leave the placeholder drive alone and let the SECOND paint -- the frame that
//       actually contains the image -- be the clean.
// (b) is what this code does, and (a) was rejected on purpose: under (a) the
// operator pays 815 ms to clean from white ONTO AN EMPTY PAGE OF PLACEHOLDER BOXES,
// and the picture itself still arrives on a differential drive -- the opposite of
// "the image page arrives on clean glass". (b) puts the from-white drive under the
// image, which is the frame being complained about, and costs nothing at all in the
// common case: the .pxc image cache means hasImagesNeedingDecode() is already false
// by the second visit, so there is no placeholder drive to sequence against. On the
// device log (r96, spine 1 = a full-page 670x1072 jpeg) the arrival was a SINGLE
// paint, which is what makes the arm-then-display order safe.
//
// One clean per (spine, page) ON ARRIVAL. The departure is now a normal page-turn
// drive: the image branch used to set `pagesUntilFullRefresh = 1`, which is what made
// leaving an image page flash a from-white GC16 over the NEXT page. That counter is
// restarted from the arrival clean instead, exactly as displayWithRefreshCycle()
// restarts it after its own FULL. See R96_ROUND.md "r96.1".
//
// r96.2: the guard is COMMITTED AT THE CONSUME SITE, not at the arm. The arm records
// the pair it is arming for; if the paint that followed did not actually take the
// flag (GfxRenderer's counter did not move), the pair is NOT committed, so the next
// paint of the same page re-arms instead of the clean silently disappearing -- the
// r96.1 failure mode the operator saw as "it cleans in neither direction".
//
// INCLUDE DISCIPLINE, because this exact file cost three flashes: an `#if FOO` on a
// macro that is not visible in the translation unit evaluates to 0 and NOTHING is
// printed. Every .cpp that reads a CP_* switch must `#include
// "platform/PlatformFeatures.h"`. EpubReaderActivity.cpp did not, so r96 and the
// 20:43 r96.1 image shipped with the whole arm compiled out (no [ERS] arm string in
// the artifact). M:/tmp/<round>-final.bin must be grepped for its own new log strings
// before it is handed over; see docs/engineering/R96_ROUND.md "the artifact check".
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CP_WAKE_ONE_DRIVE (r96.2, D2) — a wake is ONE from-white drive, full stop.
//
// r96.1 made the wake paint itself an uncancellable from-white FULL (promotion +
// r93 one-shot). On glass it still banded, and the code says why: the paint is not
// the only drive in the transition. Two things follow it over the SAME content:
//   * CP_RICKY_COMPLETION_DRIVE (GfxRenderer.cpp, "Row 9") ends any FAST paint with
//     a whole-panel GL16 drive. A DU that lands one second after the wake clean
//     therefore buys a second full-screen waveform over identical pixels.
//   * HomeActivity's first-render follow-up (cover-grid and list paths) exists to
//     repaint after thumbnails generate; on a wake it is a second full paint on top
//     of the clean that just landed.
// This switch suppresses both for exactly ONE paint after a forced clean: the row-9
// completion is skipped (nothing to complete -- the glass was cleaned by that paint)
// and the follow-up repaint is deferred to the next natural one. Set to 0 for the
// r96.1 drive sequence, verbatim.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_ONE_DRIVE
#define CP_WAKE_ONE_DRIVE 1
#endif

// ---------------------------------------------------------------------------
// CP_ENTRY_SINGLE_DRIVE (r97.2) -- a screen's FIRST drive lands on COMPLETE content.
//
// The measured defect (device log, r97.1): ActivityManager publishes the incoming
// activity (`currentActivity = std::move(pendingActivity)`) BEFORE it calls
// onEnter(), then drops the render lock. The render task can therefore wake on a
// notify that was already pending and paint the new screen while its onEnter()
// is still building the screen's content -- the list rows, the home cover thumb,
// whatever the screen loads from SD. On glass that is two whole-panel drives of
// ONE screen: a clean/GC16 over the half-built frame, then the real frame
// (measured: GC16-from-white 812 ms + GC16 807 ms entering Settings). On Home it
// is worse: the wake's armed from-white clean is spent on the cover-less frame,
// and the repaint that carries the cover arrives unarmed as a differential drive
// plus CP_RICKY_COMPLETION_DRIVE's row-9 GL16.
//
// THE MECHANISM, deliberately narrow: around onEnter() ActivityManager ARMS the
// gate and RELEASES it when onEnter() returns. While armed, displayBuffer() does
// not drive at all -- it returns immediately, records that a paint was held, and
// leaves EVERY one-shot (the armed promotion, the forced clean, the AUTO ratio,
// the diff shadow) untouched, so the drive that finally happens is built from
// the same terms it would have had. ActivityManager then repays the held paint
// with one requestUpdate(), so the screen is never left unpainted. Content that
// is ready therefore gets exactly ONE drive instead of two.
//
// BOUNDED: CP_ENTRY_GATE_TIMEOUT_MS is checked inside displayBuffer() on the same
// millis() the rest of the drive path uses. Past it the gate opens and the frame
// is driven incomplete rather than held -- a missing cover, a corrupt epub or a
// slow SD card costs one stale frame, never a blank or stuck screen. 200 ms is
// chosen because it is under a quarter of the from-white drive it is avoiding
// (~800 ms on this panel) and above the observed cost of the content setup it
// waits for (a cached Home thumb is ~0-40 ms; the Settings row build is a few
// tens of ms), so the gate normally closes long before its budget.
//
// Set to 0 for the r97.1 timing, verbatim.
// ---------------------------------------------------------------------------
#ifndef CP_ENTRY_SINGLE_DRIVE
#define CP_ENTRY_SINGLE_DRIVE 1
#endif
#ifndef CP_ENTRY_GATE_TIMEOUT_MS
#define CP_ENTRY_GATE_TIMEOUT_MS 200
#endif

// ---------------------------------------------------------------------------
// CP_SKIP_REDRIVE (r97.2) -- do not re-drive the frame a from-white clean just drove.
//
// After a clean the latch says "the glass holds exactly this frame". A paint that
// follows it with the SAME frame (a re-layout that lands on identical pixels, a
// tap that changed nothing, Home's cover-bearing frame arriving byte-identical)
// has nothing to say to the glass, so spending a whole-panel waveform on it is a
// second flash the operator reads as a band. The test is the EXISTING diff: the
// sampled PSRAM shadow (changedRatioPercent()'s baseline) is compared read-only
// against the framebuffer -- no new comparison machinery, and the shadow is never
// committed by a paint that is skipped. Unknown (no baseline yet) never skips.
// ---------------------------------------------------------------------------
#ifndef CP_SKIP_REDRIVE
#define CP_SKIP_REDRIVE 1
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_CAPTURE_MS (r96.3, D2) — how long after a resume the drive counting window
// stays open.
//
// The resume clean is asserted to be ONE from-white drive, and the operator still sees a
// band. Rather than reason about it again, the resume counts the drives it actually
// issues: one `[WAKE] drive N mode=.. err=0x.. ms=..` line per drive inside this window,
// then `[WAKE] resume drives=N`. 4 s is chosen to cover the whole transition (clean ~800 ms,
// the screen that follows, and any follow-up repaint that thumbnails or a re-layout can
// schedule) without swallowing ordinary page turns that the operator starts afterwards.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_CAPTURE_MS
#define CP_WAKE_CAPTURE_MS 4000
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_SETTLE_EXTRA_MS (r96.3, D2) — dead time between the rails coming back and the
// resume clean, ON TOP of KWAKE_SETTLE_MS (250 ms). DEFAULT 0: the shipped behaviour.
//
// This is the cheapest experiment that tells the two remaining band theories apart, and it
// must be run on glass, not reasoned about:
//   * the resume clean is a GC16-from-white that reports err=0, i.e. the drive succeeded and
//     the glass is still striped. A mid-session from-white clean does NOT band on this
//     device. The difference is the rails: at resume the TPS65185 has just come back from
//     SLEEP and the panel has been unpowered for however long the operator left it.
//   * if 800-1500 ms of extra settle removes the band, it is rail/analog settling and the
//     fix is time or a pre-charge cycle, NOT the waveform;
//   * if it does not, the waveform's first pass after power-on is the cause and waiting is
//     useless.
// The cost when enabled is exactly the extra delay on every wake (no extra drive, no extra
// flash), which is why it is a delay rather than the r82 wake-prime throwaway FULL.
// ---------------------------------------------------------------------------
// Phase 2 step D (r99.6, 3 Nov 2026): 0 -> 800 ms. This is a DISCRIMINATOR, not a fix.
// Purpose: decide whether the sleep/wake band is rail settling or frame inversion.
// If the band survives 800 ms of extra settle, settling is excluded and the cause is the
// GC16 inversion landing on a content-bearing frame -> Phase 2 A/B/C. If it disappears,
// Phase 2 collapses into a settle delay and the invalidation plans are shelved.
//
// r114d: back to 0. The 800 ms ran in r114c alongside the prime and the band survived it, so the
// discriminator answered "not settling" — the resume path now adds no dead time at all.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_SETTLE_EXTRA_MS
#define CP_WAKE_SETTLE_EXTRA_MS 0
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_SETTLE_MS (r112, F1' — docs/engineering/WAKE_REFRESH_R111.md) — the MINIMUM dead
// time, in ms, between the TPS65185 coming back (metalio_vendor_panel_repower) and the FIRST
// drive after a standby resume.
//
// F1' is "settle, don't prime": the resume already requests the right thing (one
// GC16-from-white over 684/684 rows) and still bands, so the surviving cause is that the panel
// is not settled when phase 0 starts (C1/C3). PWR_GOOD and PG==0xFA prove thresholds crossed,
// not that VPOS/VNEG/VCOM have settled; the old 250 ms was an unlogged guess. This is that
// guess made a measured, tunable deadline.
//
// It is a DEADLINE from the repower instant, not an added delay: every other resume step
// already spent inside the window counts, so the option can never quietly double the wake
// latency (and CP_WAKE_SETTLE_EXTRA_MS, when set, is absorbed rather than stacked). Applied on
// the resume-from-park path ONLY — never to an ordinary paint, Home's repaint or the auto-off
// path that is already measured clean.
//
// The CMake cache variable CP_WAKE_SETTLE_MS (idf/CMakeLists.txt + the component's
// CMakeLists.txt) is the source of truth; this block is the #ifndef floor every other switch
// in this file uses.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_SETTLE_MS
#define CP_WAKE_SETTLE_MS 500
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_ERASE_SWEEP (r114a, E1 — docs/engineering/WAKE_BANDING_ROUND3.md) — the FIRST paint
// after a standby resume runs the 20-phase all-paper erase sweep before its GC16 content drive.
//
// The diagnosis: this panel has no grayscale LUT (HalDisplay.cpp:159 returns an empty capability
// set for the ED047Tc2), so a "grayscale" frame is 16 optical levels carried by GC16 — and every
// such frame is prefixed by a real all-paper drive (my_waveform_erase_white(), 20 phases ~240 ms)
// because `from_white` is otherwise only a CLAIM that the glass already holds paper. A parked
// panel holds the 16-level sleep card, so the wake's GC16 looks every cell up as 15 -> target over
// a false premise and the mid-greys underneath are under-driven: the band. The wake paint is a
// 2-bit Home frame (< 5 levels), so it takes the content drive with no preconditioning sweep.
//
// E1 tests exactly that asymmetry and nothing else: on resume-from-park ONLY, arm a one-shot so
// the first show() forces the from-white composition (the path 16-level frames already take),
// runs the SAME sweep, disarms, and then drives the content. +~240 ms inside one show() -> no
// extra flash; the rails are already up -> no standby power cost. Never touches park, the sleep
// path, the panel drive path or the auto-off path.
//
// DEFAULT 0: the shipped image is HEAD's, byte for byte — nothing arms, nothing sweeps.
// The CMake cache variable CP_WAKE_ERASE_SWEEP (idf/CMakeLists.txt + the component's
// CMakeLists.txt) is the source of truth; this is the #ifndef floor every other switch uses.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_ERASE_SWEEP
#define CP_WAKE_ERASE_SWEEP 0
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_RAIL_SETTLE (r114d) — DEFAULT ON, 80 ms, placed BEFORE the display bus re-init.
//
// Vendor parity, and the placement is the whole point. The vendor raises P5 (CAM_SCR / the display
// rail), waits POWER_RAIL_SETTLE_MS (80), and only THEN calls bus_restore()/epd_lcd_reinit()
// (config.h:82: the settle is "before restoring the next rail OR THE DISPLAY BUS"). This tree's
// resume raised P0.5 and called epd_lcd_reinit() with zero delay, and the comment at the rail
// restore explicitly refused the wait because a 250 ms settle existed later in the path.
//
// r114c's "extra 800 ms" test was placed AFTER epd_lcd_reinit and before the drive, i.e. NOT where
// the vendor puts it, which is the most likely reason 800 ms changed nothing: the parallel bus was
// already clocked into a rail that had not settled. This is that wait, at the vendor's position.
//
// Cost: 80 ms of wake latency, zero standby cost (the rails are up and nothing is held at park).
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_RAIL_SETTLE
// r147: 1, as r131. The resume path here carries the 80 ms rail settle; only its position moved.
// r155 (shipping): the settle now runs BEFORE epd_lcd_reinit()/bus_restore(), matching the vendor
// (power_hw.cc:213-216, config.h:82: never restore the display bus before the rails have settled).
// (This comment previously described r147's inverted order, which r155 corrected.)
#define CP_WAKE_RAIL_SETTLE 1
#endif
#ifndef CP_WAKE_RAIL_SETTLE_MS
#define CP_WAKE_RAIL_SETTLE_MS 80
#endif

// ---------------------------------------------------------------------------
// CP_LOG_HOLD_DURING_DRIVE (r114d) — DEFAULT ON. No SD/flash write while a panel drive is running.
//
// Vendor parity: epd_refresh.cc:240-249 excludes the log writer (SdLogWriter_LockForDisplayRefresh)
// from panel waveforms because a flash write "pauses cache/other-core execution and starves LCD
// bounce refill" -- and a starved bounce refill drops rows, which is exactly a horizontal band.
// Our wake path emits a burst of park/resume/mark/autoflush writes around the first drive, and the
// drive-capture lines themselves are written from the render task mid-paint.
//
// The log already defers every line into a 64 KB PSRAM ring (Logging.cpp sdCapture); this switch
// only refuses the DRAIN (logSdFlush) while a drive is active and drains immediately when the drive
// returns. Nothing is dropped that the ring would not already have dropped (it keeps the newest,
// evicting the oldest, and notes that in the file), and ordering is preserved because the ring is
// append-only: the flushed block is still in the order it was logged.
// ---------------------------------------------------------------------------
#ifndef CP_LOG_HOLD_DURING_DRIVE
// r129: back to 1. r128 hardcoded 0 because the panel shim had no hook to install (the r105/r114d
// gate was removed with the rest of the experiment scaffolding), so 0 was the only value the tree
// would link with. The hook is restored in r129 and the CMake option emits the definition, so the
// default here only matters when the tree is built without the component's CMakeLists.
#define CP_LOG_HOLD_DURING_DRIVE 1
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_RAW_CLEAR + CP_WAKE_PRIME_MODE (r114c C2, reordered in r114d).
//
// THE COLD-BOOT PRIME, now issued BEFORE the first content drive. Cold boot does, in order:
// epd_init -> epd_set_vcom -> my_waveform_init -> epd_hl_init -> epd_poweron -> epd_clear ->
// epd_poweroff ... and the first app paint then re-powers and drives. epd_clear() is NOT a
// waveform and not a LUT drive: 3 x (10 dark + 10 white + 3 white) = 69 full-panel scans through
// epd_push_pixels() with EPD_MODE high (epdiy.c:37 -> render.c:347 -> epd_clear_area_cycles,
// render_lcd.c:354). So on cold boot the panel is physically white before any LUT drive, and on a
// resume (r110..r114b) the first LUT drive landed on the sleep image instead.
//
// r114c proved the prime works but ran it from inside the first wake paint, so a content drive
// preceded it: on glass, banded drive -> 69-scan clear -> clean content drive. r114d is an
// ORDERING change only: main.cpp calls metalio_vendor_panel_wake_prime() synchronously on the
// resume path, after the bus restore and the rail repower and before the Home transition can
// paint. Sequence on wake: prime -> TPS off/on -> exactly ONE content drive.
//
// CP_WAKE_PRIME_MODE selects the scan shape. Scan counts are total full-panel scans; the 3-cycle
// value is the one proven on glass and the one r114d ships. Change ONE value to try a cheaper
// prime — do not restructure anything else.
//   0  none       0 scans   (compile-out path: no prime, r114b behaviour)
//   1  white-only 13 scans  (10 white + 3 white tail; skips the dark phase)
//   2  black-only 10 scans  (dark phase only; content drive then starts from black, not white)
//   3  1cycle     23 scans  (one full 10 dark + 10 white + 3 white)
//   4  3cycles    69 scans  (DEFAULT — identical to cold boot's epd_clear())
//
// Cost: ~0.9-1.7 s of flashing on wake (measured per mode at runtime in the `[WAKE] prime` line).
// Zero standby cost: the rails are already up when it runs, nothing is held during park. Net
// latency vs r114c is a WIN even so: r114c wasted a whole content drive (~1.2 s) before the prime.
//
// The CMake cache variables CP_WAKE_RAW_CLEAR (on/off) and CP_WAKE_PRIME_MODE (0..4) are the
// source of truth; these are the #ifndef floors every other switch in this file uses.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_RAW_CLEAR
// r128: 0. No wake prime runs (see metalio_vendor_panel_wake_prime: removed with r104's resume).
#define CP_WAKE_RAW_CLEAR 0
#endif

#define CP_WAKE_PRIME_NONE 0
#define CP_WAKE_PRIME_WHITE_ONLY 1
#define CP_WAKE_PRIME_BLACK_ONLY 2
#define CP_WAKE_PRIME_1_CYCLE 3
#define CP_WAKE_PRIME_3_CYCLES 4
#ifndef CP_WAKE_PRIME_MODE
// r128: NONE. The prime entry point is gone from the panel shim; this is inert.
#define CP_WAKE_PRIME_MODE CP_WAKE_PRIME_NONE
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_WARM_REDRIVE (r114f) — DEFAULT 1 (EQUALIZE). A resume-only warm-up drive of the canvas
// ALREADY on the glass, issued BEFORE the wake prime, so the panel's weak first-drive window is
// spent on pixels that are already there instead of on the frame the reader is about to see.
//
// The canvas is unchanged by it: EQUALIZE_BLACK re-inks the black-target cells over the whole panel
// (no erase pass, no white flash — the retained frame stays legible), and FULL is the from-white
// GC16 fallback that also charges every cell. Both leave the glass showing what it already showed.
//
//   0  off               (r114d, byte for byte)
//   1  EQUALIZE_BLACK    (DEFAULT — invisible whole-panel re-ink)
//   2  FULL              (from-white GC16; charges the panel, flashes)
//
// Resume only, so ZERO standby cost: nothing is driven at park and nothing is driven after the
// content drive. The CMake cache variable of the same name is the source of truth; this is the
// floor, and its constants are the values main.cpp compares against.
// ---------------------------------------------------------------------------
#define CP_WAKE_WARM_REDRIVE_OFF 0
#define CP_WAKE_WARM_REDRIVE_EQUALIZE 1
#define CP_WAKE_WARM_REDRIVE_FULL 2
#ifndef CP_WAKE_WARM_REDRIVE
// r128: OFF. No warm-up re-drive exists on the resume path any more; this is inert.
#define CP_WAKE_WARM_REDRIVE CP_WAKE_WARM_REDRIVE_OFF
#endif

// ---------------------------------------------------------------------------
// CP_WAKE_FULL_SPEED (r115) — REMOVED IN r116. The resume full-speed hold and the two esp_pm locks
// that went with it are deleted from source (main.cpp), not flagged off: r116 compiles
// CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE out — the only state difference between the
// clean r104 and the banded r105 — and the clock window became meaningless once esp_pm stopped
// being a second owner of the CPU frequency. Nothing reads this name any more, so it has no
// definition here: re-adding it would silently reintroduce a half-built path.
// ---------------------------------------------------------------------------

// CP_WAKE_ONE_DRIVE_STRICT (r114b, E2 — docs/engineering/WAKE_BANDING_ROUND4.md) — the FIRST
// paint after a wake's forced clean takes NO row-9 completion drive, whatever its pixels say.
// DEFAULT 0: HEAD's sequence, byte for byte.
//
// CP_WAKE_ONE_DRIVE already suppresses the completion for that one paint, but only when
// `sameAsShadowAtEntry == 1` (GfxRenderer.cpp r101.13b, "row 9"): the paint must be a
// whole-frame memcmp match against the frame the clean drove. On a wake the paint that
// follows the clean is normally a Home/reader frame whose COVERS or page text arrive after
// the clean, so the compare is 0, `skipCompletion` is false, and row 9 appends a second
// whole-panel drive inside the wake window -- the band the operator photographs. The
// r101.13b identity term exists to stop an UNRELATED fast transition losing its completion;
// it was never meant to re-enable one one paint after a from-white clean, when the glass has
// just been cleaned and the completion has nothing to finish.
//
// When ON: `postCleanPaint` (g_paintsSinceClean > 0, !forceClean) forces skipCompletion,
// independently of the identity compare, for exactly one paint. Nothing else changes: same
// mode, same timing, no extra drive, no arm, no timer. The strict skip is strictly fewer
// drives, so standby power is untouched (the rails are already up and the decision happens
// inside a paint that would have driven anyway).
//
// Risk (why this is a switch and not the default): a genuinely DIFFERENT screen arriving as
// the first paint after the clean loses its GL16 finish -> possible ghosting on that one
// transition, which is the r101.13b regression. Measure on glass before defaulting it.
// ---------------------------------------------------------------------------
#ifndef CP_WAKE_ONE_DRIVE_STRICT
#define CP_WAKE_ONE_DRIVE_STRICT 0
#endif

// ---------------------------------------------------------------------------
// CP_LPSLEEP_ALLOW_HP_SLEEP (r96.2, D3) — may the `lpsleep` bench put the HP core
// into a real light sleep?
//
// r96.1 `lpsleep 60` armed, answered, and the device never came back; the shell was
// gone for >2 minutes and the arm record never reached the card. The one thing that
// slice did and standby does NOT do is run the LP core with the HP core asleep and a
// ULP wake armed. Standby's slice (LP core stopped, EXT1 + RTC timer) is proven on
// this exact device for hours at a time, so the bench now uses that path: the LP
// program is sampled, STOPPED, the slice runs, and the program is restarted after to
// prove the core still works.
//
// Set to 0 to make `lpsleep` refuse outright with a stated reason and leave the
// measurement to `lpbench` (LP core, HP awake). That is the hard off switch if a
// future round cannot establish a bounded wake source.
//
// r96.3 (D3): NOW 0, and the reason is not the one r96.2 assumed. The cheap hypothesis
// was that `lpsleep` enters sleep through esp_light_sleep_start() and therefore bypasses
// the vendor-wrapped pmu_sleep_start() that performs the PSRAM->XTAL clock handoff. It is
// FALSE: LpCoreService.cpp:476 calls metalio_sleep_light_sleep(), the SAME entry point
// metalioStandbyLoop() uses (main.cpp:1280), and that function is the only place
// s_restore_armed is set for __wrap_rtc_clk_mpll_configure(). Verified in the source, both
// call sites, this round.
//
// What the device log DOES show is that a slice is not inherently fatal: one r96.2 boot
// wrote `lpsleep: pass sec=60 slept=60000ms asleep=60049471us cause=timer` -- twelve 5 s
// chunks, every wake a timer, err=0. But a re-run in this round (r96.3, `lpsleep 15` on
// the same device) left NO lpsleep record at all: not the per-chunk "armed", not "chunk
// back", not the pass line. The command answered ("armed 15 s, runs in ~1 s") and the
// device then stopped answering the shell entirely; the only recovery was closing the
// serial port, which resets the board. So the failure is NOT deterministic and does not
// always even reach the first sleep call -- it can die before startBench() returns, i.e.
// before the first flushed record. The USB-Serial/JTAG link dying across the slice is one
// candidate for the runs that do reach it; nothing here proves it.
//
// The entry paths also still differ in one respect that matters: standby masks the IO
// expander so ONLY the power key can assert INT (ioexp_mask_for_sleep +
// ioexp_enable_power_wake_pin) and calls halClock.disableAlarm(); this bench only releases
// the accel line. Until the entry path is identical to standby's AND a slice has been
// observed to leave the shell answering, the command stays refused -- it has stranded the
// operator's device twice and must not be runnable by accident.
// ---------------------------------------------------------------------------
#ifndef CP_LPSLEEP_ALLOW_HP_SLEEP
#define CP_LPSLEEP_ALLOW_HP_SLEEP 0
#endif
