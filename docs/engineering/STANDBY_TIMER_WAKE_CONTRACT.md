# Standby: the timer-wake contract (peripherals stay parked)

**Status:** reference contract. No behaviour was changed to produce it.
**Date:** 5 Nov 2026 (Step 2.5, item 6 — vendor triage `docs/research/VENDOR_UPSTREAM_TRIAGE_2026_11_05.md`).
**Why it exists:** it is the contract that has to be in hand before `LPSLEEP` is re-opened
(Step 4 / `PWR6`), so the next round knows exactly what a timer wake is allowed to touch.

## The vendor contract

Source: `vendor-metalio` @ `2b2c36a`, `main/boards/common/power_policy/power_hw.cc` and
`power_policy/README.en.md`.

> A timer wake does not leave standby. […] They do not remove the overlay, resume application
> networking, start recording, restore peripheral supplies or the display bus, or remount SD.
> Another sleep sequence runs with peripherals still powered down unless the power-off deadline or
> a key-exit request requires action.
> — `README.en.md`, “Timer wake: stay in standby”

Their code enforces it structurally rather than by a flag: restoration is a single
`power_hw_*_restore()` stage reached only from the exit path, and the timer branch returns to the
sleep sequence before it. A slice therefore costs the MCU's sleep-entry and wake-arm work and
nothing else — no panel rail, no PA rail, no SD mount, no sensor re-init.

The proposal on the table for `PWR6` is a longer/deeper sleep (`LPSLEEP`). Anything that wakes
peripherals on a slice boundary would be paid **per slice**, i.e. 60 times an hour, which is why
this is a contract and not a preference.

## What a timer wake currently does in our tree

All references are `src/main.cpp` unless stated; numbers are as of this tree on 5 Nov 2026 with
concurrent settings work applied (symbols are the durable reference). The standby loop is
`metalioStandbyLoop()` (`:1370`); the decision point is `:1550`.

| Peripheral | Parked here | Restored here | On a **timer** wake |
|---|---|---|---|
| Panel + TPS65185 rails | `display.deepSleep()` `:1762`, `tps_enter_sleep()` `:1766`, 16 EPD pads latched low `:1787` | `metalio_vendor_panel_repower()` `:1935`, `bus_restore()` `:1924`, `display.wake()` `:1925` | stays parked — the timer branch `continue`s before any of it |
| PA / BT rail (P1.4) | `pa_rail_set(false)` `:1823`, gated by `CP_CUT_PA_RAIL_ON_STANDBY` (default OFF) | `pa_rail_set(true)` `:1908` + 80 ms settle, before the panel repower | stays parked. The per-slice expander mask/restore pair (`:1401`, `:1541`) rewrites the P1 latches **from the shadow `s_p1_out` masked with `CFG_P1_RAIL_MASK`**, so a cut latch is carried across the slice (`epd_board_metalio_eink4_plus.c:690-700`); only pin *directions* change |
| SD card | `Storage.prepareForDeepSleep()` `:1847`, gated by `CP_SD_PARK_ON_STANDBY` | `Storage.begin()` `:1883`, one retry at `:1886` | stays unmounted; nothing between the sleep call and the timer `continue` opens the card (`sleepJournal()` lines are dropped while parked by design, `:1842`) |
| Sensors (IMU INT1) | INT routing cleared per slice: `releaseAccelIntLine()` `:1400`, `CTRL_REG3=0`, `MetalioEInk4PlusBoard.h:652` | n/a — this build never brings the IMU up | one I²C write (two per pass: once at `:1400`, once inside `waitForInterruptLineIdle()`, `MetalioEInk4PlusBoard.h:581`). No init, no rail |
| Touch (FT6336U) | `gpio.prepareForDeepSleep()` `:1759` (hibernate, 0xA5) | `gpio.resumeAfterSleep()` on the resume path | stays hibernated |
| Frontlight | `Frontlight.setOn(false)` `:1757` | `Frontlight.setOn(true)` `:1927` from the saved state | stays off |
| Other expander latches (P0.5 camera/screen power, P0.4 PA_EN) | not cut at park (single-rail cut unproven, see `:1789-1793`) | — | re-written each slice with the same values by `p0_output_value()` (`epd_board_metalio_eink4_plus.c:204-219`), so there is no transition, only a redundant I²C write |

Everything between `metalio_sleep_light_sleep()` (`:1498`) and the `continue` is clock/CPU work plus
key-direction and wake-source bookkeeping: `metalio_sleep_*` is the SDK MPLL/PSRAM handoff only
(`idf/components/metalio_driver/sleep/metalio_sleep.c:216-232`), `ioexp_restore_keys()` restores
*directions*, `sentryEnd()` gives the GPIO back.

## Conformance: we already conform

Verdict: **PASS — no standby change needed.** A timer wake in this tree restores nothing. The
restore path is a straight line below `metalioStandbyLoop()`'s `return` (`:1597`, `wake hold=…`)
— `metalioStandbySleep()` resumes at `:1867` — and the SD remount, the PA rail and the panel
repower all sit after it. That is the same shape the vendor gets from their single-stage restore:
theirs is one function per stage, ours is one `return`.

The two paths that are *not* user exits behave the same way:

- **spurious EXT1 wake** (charger `PWR_GOOD` is reachable because P0.7 stays an input — see the
  note at `epd_board_metalio_eink4_plus.c:622-639`): `metalioVerifyStandbyWake()` finds no press,
  journals `spurious wake` (`:1586`), and `continue`s. Peripherals stay parked; the cost is one
  extra wake+sleep per charger event, not a re-init.
- **slice rejected** (`ESP_ERR_SLEEP_REJECT`, `:1546`): retry, parking untouched.

### Guard added at the decision point

`src/main.cpp:1550` now carries the contract as a comment naming each peripheral and its
park/restore line, plus one assertion:

```cpp
if (cause == ESP_SLEEP_WAKEUP_TIMER) {
  if (!standbytrim::parked()) { /* rate-limited journal, counter slpParkViolations */ }
  continue;
}
```

`standbytrim::parked()` (`src/platform/StandbyTrims.h:51`) is true from `setParked(true)` at
`:1804` until `setParked(false)` at `:1917`, i.e. across the whole standby — including every
slice. A timer wake that finds it false means something on the slice path restored peripherals,
which is precisely what this contract forbids. Counter: `slpParkViolations` (`:672`), journal line
`contract: timer wake with the park lifted (parked=0) …`. It costs one RAM read per slice and
should read 0 for the life of a build.

**Residual gap, stated plainly:** the guard is software-state only. It cannot see a
peripheral that was re-initialised without the trim flag being involved (e.g. a future commit
calling `epd_poweron()` or `Storage.begin()` inside the loop before the `continue`). A hardware
assertion would need a TCA P1 read-back per slice; at 60 slices/hour that is not obviously free
and was deliberately not added. If `LPSLEEP` work starts, the guard to add is a P1 output read-back
compared against the shadow, not a deeper software flag.

## What this means for `PWR6` / `LPSLEEP`

- A slice is already “cheap” in exactly the sense the vendor means: sleep-entry work, the
  per-slice expander direction flip, one IMU I²C write, and the journal/sentry bookkeeping.
- The panel, PA rail, SD and touch are only touched on the exit path. Any `LPSLEEP` design that
  moves a `power_hw`-style restore *before* the exit decision breaks this contract and pays it
  60×/hour.
- The one thing that is per-slice and could grow under `LPSLEEP` is the expander mask/restore
  pair plus `waitForInterruptLineIdle()` (which itself re-routes the IMU). If slice cost is
  revisited, that is the place to measure — not the rails.

Evidence method: code reading against `main.cpp` at this commit; **not** measured on glass. The
conformance claim is a code-path claim, and the current figures for standby current remain the
ones in `docs/DEEP_SLEEP_FAST_RESUME_AUDIT.md`, not a new measurement.
