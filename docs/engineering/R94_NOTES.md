# r94 notes — HOME1, RICKY2 C4, ABOUT1

Three code items on top of r93 (`604ce334`, `5c516539`, `8c6cc4cf`, `6c8a56e5`) plus IMG2
(`b968b0a7`, carried unchanged). No behaviour change is intended by any of them; the only
user-visible change on the glass is the About screen's values.

---

## C4 — the refresh DECISION log line (`lib/GfxRenderer/GfxRenderer.cpp`)

Row 23 of the pipeline already logged the *result* (`paint req=… applied=… result=… rails=…`).
C4 adds **one** DEBUG line per paint, emitted **before** the drive, carrying the inputs both
policy stages consumed. Blocking and async paint paths both emit it. Nothing branches on it.

```
paintdec req=%d res=%d force=%d ratio=%d shadow=%d auto=%d promo=%d mode=%d valid=%d clean=%d prev=%d end=%d
```

| field | meaning | how to read it |
|---|---|---|
| `req` | mode the **call site** asked for (0 FULL, 1 HALF, 2 FAST, 3 GL16) | what the screen wanted |
| `res` | mode **our producer** resolved it to, before the RickyOS policy stage | differs from `req` when an AUTO (measured ratio) or an explicit promotion was armed |
| `force` | 1 = the **r93** one-shot clean-on-arrival term (`forceCleanNextRefresh`) was set for this paint | a reader→Home transition; expect `force=1 end=0` exactly once |
| `ratio` | changed-pixel **percent** measured for THIS paint; **-1 = nothing measured** (no AUTO was armed) | < 5 (the AUTO threshold) ⇒ `res=2` (DU); ≥ 5 ⇒ `res=1` (HALF) |
| `shadow` | 1 = a previous-frame diff baseline existed when the paint started | 0 on the first paint after boot / a failed drive / a park, i.e. a diff would be meaningless |
| `auto` | 1 = an AUTO was armed at entry (an awaited frame measured its own ratio) | the armed-ratio producer |
| `promo` / `mode` | 1 = an explicit promotion was armed at entry / the mode it asked for | `requestUpdateAndWait`-style follow-up frames |
| `valid` | 1 = the latch describes what is on the glass (a drive succeeded since boot/park) | 0 ⇒ `clean` is also 1 |
| `clean` | 1 = the panel is known-clean (never painted since boot, deinit or park) | policy rule 1 forces GC16-or-stronger |
| `prev` | the latch value the policy was handed: 0 clean, 1 fast applied, 2 from-white FULL applied | `prev=2` + a fast `res` ⇒ rule 3 promotes this paint to HALF, once |
| `end` | the mode actually resolved for the drive — the policy's output | this is what reaches the panel |

Attribution recipes, from the log alone:

* `ratio` climbed but `end` stayed 2 and the glass ghosted → the 5 % AUTO threshold (row 21), not the policy.
* `prev=2` followed by `end=1` on the next paint → rule 3 (no HALF/DU after a FULL); it happens **once** by construction.
* `clean=1` on a `req=2` paint → rule 1 promoted it; if that was unexpected, the latch was dropped (park, failed drive, boot).
* `force=1` more than once per reader→Home transition → the r93 consumer is broken; `end` should be 0 on that one paint.

`ratio=-1` is not a missing measurement to "fix": the ratio is only measured when an AUTO is armed,
and re-measuring it for a log line would commit the diff baseline twice and change the *next*
paint's decision. That is why the value is captured where it is already computed, not recomputed here.

Field `ms` deliberately does **not** appear: the async path has no measured wall time (the drive is
deferred). Wall time stays on the existing row-23 line, which is unchanged.

**Logging-only proof:** the diff adds one file-scope `int g_paintDecisionRatio` (written where the
measurement already happens), five `[[maybe_unused]] const` snapshots per path, and the two
`LOG_DBG` calls. No branch, no ordering change, no new state in the resolution, nothing at INFO.

---

## HOME1 — `homeMenuBand()` no longer overruns the panel

`src/activities/home/HomeActivity.cpp`. The band was `y = homeTopPadding + homeCoverTileHeight +
homeMenuTopOffset` (433 px on the Plus: 65 + **342** + 26 — `homeCoverTileHeight` is a *Physical*
metric and is not density-scaled) but `height = pageHeight − (headerHeight + homeTopPadding +
verticalSpacing + homeMenuTopOffset + buttonHintsHeight)` = **1057**, so the rect ran 433…1490 on a
1216 px panel. It now subtracts the three terms that *position* it:

```
height = pageHeight − (homeTopPadding + homeCoverTileHeight + homeMenuTopOffset)   // 783; 433 + 783 = 1216
```

### Consumer audit (every reader of the band's `height`)

| consumer | reads `band.height`? | why the output cannot change |
|---|---|---|
| `HomeActivity::loop()` (`MenuLayout`, line ~539) | no — passes the band to `getMenuLayout` | |
| `HomeActivity::render()` (line ~648) | no — passes it to `drawButtonMenu` | |
| `BaseTheme::getMenuLayout` | **no** (`band.y` only) | rows/pitch come from metrics |
| `BaseTheme::drawButtonMenu` | **no** (`rect.x/width`, `layout.rowHeight`) | |
| `AndroTheme::getMenuLayout` | **no** — uses `band.y` and `renderer.getScreenHeight()` | the Andro pitch/spread is derived from the screen bottom, never the rect bottom |
| `AndroTheme::drawButtonMenu` | **no** | scroll indicator uses `layout.rows * layout.step` |
| `LyraTheme::drawButtonMenu` | **no** | |
| `RoundedRaffTheme::getMenuLayout` | **yes** — `pageItems = band.height / step` | 976/55 = **17** → 762/55 = **13**; the home menu has at most 6 rows, so `rows = min(pageItems, 6) = 6` and `firstIndex = 0` in both cases |
| `RoundedRaffTheme::drawButtonMenu` | **yes** — `rect.height / step`, "for the scroll bar only" | `drawScrollBar()` returns immediately when `itemCount <= pageItems` (6 ≤ 11 in both readings), so no bar is drawn either way |

No consumer's behaviour changes; the two that read `height` are bounded well away from the value
where it could matter (a 6-row menu against a 17-row or 13-row page). Painted output is identical;
the rect now ends exactly on the panel's last pixel instead of 274 px past it.

---

## ABOUT1 — Settings → About, field by field

`src/activities/settings/AboutActivity.cpp` (+ `src/display/PanelIdentity.h`, new). The layout,
typography and row chrome are untouched (r68–r70 fonts, r77/r78 Andro overrides); only the values
are filled, and four rows were added for hardware the screen had no row for.

| row | rendered value (Plus) | source |
|---|---|---|
| Device | `Metalio E-Ink 4 Plus` | `BoardConfig::ACTIVE.name` (runtime profile) |
| Firmware | `MetalioEinkPlus r94` | `CROSSPOINT_VERSION` (build define) |
| **Build** (new) | `Nov  1 2026 17:19:12` | `__DATE__ " " __TIME__` |
| Chip | `ESP32-S31 2c rev x.y` | `esp_chip_info()` (model/cores/revision) + `CONFIG_IDF_TARGET` fallback |
| Flash | `16 MB` | `esp_flash_get_size(nullptr, &bytes)`; `unknown` if the call fails |
| **PSRAM** (new) | `16 MB` | `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`; `none` when 0 |
| Display Controller | `ED047TC2_1216 E0470A14-AF` | `displayControllerName(ACTIVE.displayController)` → `CP_PANEL_CONTROLLER_NAME` for `Ed047Tc2`; vendor part from `PanelIdentity.h` |
| **Panel PMIC** (new) | `TPS65185 0x68/TCA9555` | `PanelIdentity.h` name + `freeink::metalio4plus::TPS65185_ADDR` / `EXPANDER_ADDR` |
| Resolution | `1216x684` | `ACTIVE.displayWidth × displayHeight` (panel-native scan frame) |
| Touch | `FT6336U 0x38 GPIO5` | `touchControllerName(ACTIVE.touch.controller)` + `touch.i2cAddress` + `touch.irq` |
| Frontlight | `Yes` | `Frontlight.present()` (runtime) |
| RTC | `PCF8563 0x51` | `sensors.rtcType` switch + `sensors.rtcAddr`, gated on `halClock.isAvailable()` |
| Tilt Sensor (IMU) | `SC7A20H 0x19` | `sensors.imuType` switch + `sensors.imuAddr`, gated on `halTiltSensor.isAvailable()` |
| **Fuel Gauge** (new) | `BQ27220 0x55` | `batteryGauge.gaugeType` switch + `gaugeAddr`; `none (ADC)` when `gaugeAddr == 0` |
| MAC Address | `xx:xx:…` | `esp_read_mac(ESP_MAC_WIFI_STA)` |

Every part name is a `switch` on the board profile's own enum, and every address comes from the
profile or the board header, so a row cannot claim a part the profile does not declare. Two values
are genuinely not header-sourced:

* **`E0470A14-AF`** — the tree identifies the panel by the C symbol `ED047TC2_1216` / `E0470A14_AF`
  (`idf/components/metalio_driver/display/displays.c:67,76`), never by a string macro. The name
  therefore lives in `src/display/PanelIdentity.h` with the symbol it names; a panel revision is a
  one-file change there. The geometry printed beside it still comes from the runtime profile.
* **`unknown`** — a non-Plus board prints `unknown` for Panel PMIC, because `BoardConfig` has no
  PMIC field and naming a part would be a guess. On the Plus the row is always populated.

`PCA9555` in the backlog row vs `TCA9555` on this board: the board header
(`freeink-sdk/.../MetalioEInk4PlusBoard.h`) and the driver (`metalio_driver/bus/pca9555.c`) both
address a **TCA9555** at 0x20, so that is what the row prints.
