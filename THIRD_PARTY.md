# Third-party components and licences

Inventory of everything in this repository that is not original CrossPoint code, with the licence it
arrives under and whether redistribution is permitted. The root `LICENSE` (MIT, © 2025 CrossPoint
Reader organization) covers CrossPoint's own code; it does **not** relicense the components below.

Every component listed is redistributable. The two items that were **not** redistributable are
excluded from this snapshot and named at the bottom.

## Bundled in this repository

| Path | Component | Licence | Redistributable | Compatible with MIT root |
|---|---|---|---|---|
| `LICENSE` | CrossPoint Reader firmware (app code) | MIT, © 2025 CrossPoint Reader organization | Yes | — (root) |
| `src/`, `lib/`, `idf/components/crosspoint_app` | this fork's application code | MIT (same root) | Yes | — |
| `freeink-sdk/` (submodule) | FreeInk SDK, © 2026 FreeInk | MIT | Yes | Yes |
| `freeink-sdk/libs/…` | portions derived from the OpenX4 E-Paper Community SDK | MIT, © 2025 Open X4 E-Paper Contributors | Yes | Yes |
| `freeink-sdk/libs/assets/Icons/lucide` (submodule) | Lucide icons | ISC, © Lucide Icons and contributors | Yes | Yes |
| `freeink-sdk/libs/book/FreeInkBook/third_party/expat` | Expat | MIT (expat terms) | Yes | Yes |
| `freeink-sdk/libs/book/FreeInkBook/third_party/libunibreak` | libunibreak | zlib/MIT-style | Yes | Yes |
| `freeink-sdk/libs/book/FreeInkBook/third_party/miniz` (+ ContentProtection copy) | miniz | MIT, © RAD Game Tools / Valve / Rich Geldreich | Yes | Yes |
| `freeink-sdk/libs/book/FreeInkBook/third_party/pngle` | pngle | MIT, © 2019 kikuchan | Yes | Yes |
| `freeink-sdk/libs/book/FreeInkBook/third_party/hyphen-patterns` | hyph-utf8 patterns | permissive (hyph-utf8 terms) | Yes | Yes |
| `freeink-sdk/libs/font/FreeInkFont/third_party/freetype` | FreeType | FTL / GPLv2 dual | Yes (FTL chosen) | Yes |
| `idf/components/metalio_driver/` | vendor display/power driver, EPDiy-derived | **LGPL-3.0** (see note) | Yes | Yes — separate work, own licence |
| `idf/components/metalio_driver/display/waveforms/my_gray8_*.h`, `LICENSE.gray8` | E0470 grey waveform tables | Apache-2.0, © 2026 mindreset | Yes | Yes |
| `idf/components/extern/AnimatedGIF` | AnimatedGIF | Apache-2.0, © 2020 BitBank Software | Yes | Yes |
| `idf/components/extern/JPEGDEC` | JPEGDEC | Apache-2.0, © 2020 BitBank Software | Yes | Yes |
| `idf/components/extern/PNGdec` | PNGdec | Apache-2.0, © 2020 BitBank Software | Yes | Yes |
| `idf/components/extern/SdFat` | SdFat | MIT, © 2011–2020 Bill Greiman | Yes | Yes |
| `idf/components/extern/WebSockets` | arduinoWebSockets | **LGPL-2.1** | Yes | Yes — library, licence retained |
| `idf/components/extern/WebSockets/src/libb64` | libb64 | public-domain dedication | Yes | Yes |

Apache-2.0 components are one-way compatible into an MIT-licensed distribution provided their
notices are kept, which they are (each directory carries its own `LICENSE`). LGPL components are
distributed as separate library code with their licences intact; the source of every one of them is
in this repository, which satisfies the source-availability obligation.

### `idf/components/metalio_driver/` — LGPL-3.0 position

This component is the board's panel and power code, taken from the vendor's own firmware tree. Its
file set, API and structure are EPDiy's (`epdiy.c/h`, `highlevel.c`, `lut.c`, the `epd_*` types),
and upstream EPDiy (<https://github.com/vroland/epdiy>) is licensed **LGPL-3.0** — verified against
the licence text in that repository, not assumed.

The copies here arrived with their licence headers stripped; there is no copyright line in any of
the `.c`/`.h` files. Because the licence of a derivative work does not disappear when its header is
removed, this repository treats the component as LGPL-3.0, ships the upstream licence text at
`idf/components/metalio_driver/LICENSE.LGPL-3.0`, and keeps it in a separate directory from the MIT
application code so the boundary is visible. Nothing here claims authorship of it.

Practical note for binary distributors: LGPL-3.0 §4 asks for the Corresponding Source and, for a
"User Product", installation information sufficient to install a modified version. That is
satisfiable on this device (source is here; flashing is `esptool` over USB-C), but it is your
obligation when you hand someone a `.bin`, not the repository's.

## Excluded from this snapshot — do not re-add without a licence

| Item | Why it is not here |
|---|---|
| `idf/components/metalio_driver/display/firmware/ft6336u_v03_app.bin` (+ its README) | Vendor-supplied FT6336U touch-controller firmware (`LT0470GF0430M_V1.1 _FT6336U_684x1216_FT6336U_V03_20261010_app.bin`, redistributed by the panel supplier). No licence grant accompanies it, and **nothing in this tree references it** — no Kconfig option, no embed rule, no source symbol. It was removed rather than published. If you need the touch firmware upgraded, obtain it from the panel vendor and use their tool. |
| `baselines/` | Pre-built images and per-round build logs from development. Contains local absolute paths and multi-MB binaries; not part of the source. |
| `docs/images/spiflash/crosspoint_spiflash_backup.tar.xz` | A full SPI-flash dump of a physical unit — device-specific calibration and user state, not redistributable content. The surrounding brick-recovery photos are kept. |
| `nix/` | Linux dev-shell helper; `nix/default.nix` is a symlink to `shell.nix`, which Windows cannot represent. Not needed for the firmware build. |
| `idf/components/arduino/` | The Arduino core (LGPL-2.1, 79 MB). Not vendored — see the README's prerequisite 3. |
| `idf/managed_components/` | ESP-IDF component-manager output, regenerated by `idf.py` from `idf/components/crosspoint_app/idf_component.yml`. |

## Toolchain you supply (not redistributed here)

| Tool | Licence |
|---|---|
| ESP-IDF 6.1 | Apache-2.0 |
| arduino-esp32 4.0.0-RC1 | LGPL-2.1 |
| Arduino-wolfSSL 5.7.2 | GPLv2 |
| esptool | GPL-2.0 |
