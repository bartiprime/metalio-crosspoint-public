# CrossPoint Reader — Metalio E-Ink 4 Plus

CrossPoint Reader firmware for the **Metalio E-Ink 4 Plus** (ESP32-S31, 4.7″ 1216×684 e-paper).
This repository is a source snapshot of the fork that runs on that board — not a generic CrossPoint
build, and not upstream's device list. If you have a different device, take upstream
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) instead: the panel
timings, power sequencing and wake behaviour here are tuned to this hardware and only this hardware.

Root licence is MIT (inherited from CrossPoint). Some components carry their own licences — read
[THIRD_PARTY.md](THIRD_PARTY.md) before you redistribute a binary.

---

## The hardware

| Part | What it is |
|---|---|
| SoC | **ESP32-S31** (RISC-V, `riscv32-esp-elf` toolchain), 16 MB flash, octal PSRAM at 200 MHz |
| Panel | **E0470A14-AF**, 4.7″ e-paper, **1216×684**, parallel (LCD-mode) interface |
| PMIC | **TPS65185** e-paper bias supply, gate/source sequencing over I²C |
| GPIO expander | **PCA9555** — panel control lines and the vibration motor hang off it |
| Touch | **FT6336U** capacitive, I²C @ `0x38`, INT on GPIO5 |
| Frontlight | I²C-controlled, on the same bus as the PMIC and sensors |
| Motion | **SC7A20H** accelerometer @ `0x19`/`0x18` (tilt page-turn, orientation) |
| Power | **BQ27220** fuel gauge @ `0x55`; **PCF8563** RTC @ `0x51` |
| Audio | **BH1098G** audio module on UART1 (TX39/RX38), speaker PA with a switchable rail |
| Storage | microSD on SDMMC slot 0 (dedicated IOMUX pins, not the default GPIO matrix) |
| Port | USB-C — USB-Serial-JTAG (flashing + console) |

The board is deliberately power-sequenced: the panel rails, the SD card, the audio PA and the
camera rail each have their own standby policy, and light sleep has to hand the PSRAM clock back to
the MPLL on resume. Most of what looks odd in `idf/components/metalio_driver/` exists because this
board will not boot, or will not wake, without it.

## What works

- **Reader** — EPUB 2/3, `.xtc`, `.txt`. Chapter navigation, bookmarks, dictionary (StarDict),
  hyphenation, kerning, embedded-style option, images, footnotes.
- **Library / browser** — file browser over the SD card, catalogue view, sort and filter.
- **Both themes** — light and dark UI, e-ink-aware repaint paths (partial where it is safe,
  full/GC16 where it is not).
- **KOReader sync** — progress sync against a KOReader-compatible sync server, with per-server auth
  configured in Settings.
- **Flick page turn + calibration** — tilt-to-turn using the accelerometer, with a calibration
  activity (`Settings → Flick calibration`), because the mounting orientation differs per unit.
- **TTS / audio** — on-device speech through the BH1098G module, with UI sounds (page turn, menu
  move, select, back, error) governed by a one-deep queue.
- **Battery statistics** — BQ27220 fuel gauge, with a statistics screen.
- **Sleep / standby** — light sleep with parked peripherals, RTC wake, and a resume path that
  re-primes the panel instead of leaving it blank.
- **Wi-Fi** — network settings, a web server activity, and a USB shell for on-device diagnostics.

Anything not in that list should be assumed untested on this board rather than working.

## What does not work / known issues

- **Cold boot flashes white before the first paint.** Known, pre-existing behaviour: the panel comes
  up uncharged and the first frame is a clear. Not introduced by this snapshot, and not something
  to "fix" by adding a splash — the boot splash option (`CP_BOOT_SPLASH`) exists but is **off** in
  the shipping configuration.
- **UI sounds can miss the first click after an idle period.** The audio path is brought up lazily;
  the first event after the module has parked can be silent while it resets. Later sounds are normal.
- **Some screens are intentionally silent** — no UI sound while a paint is in flight, and on screens
  that never register a nav event.
- **No release binaries.** Source only. A build from here is untested until it is flashed and
  checked on the glass.
- **Only the Windows/MSYS2 build path is verified.** Linux and macOS `idf.py` paths are untested
  with this tree.
- Behaviours not listed under "What works" have not been verified on hardware for this snapshot.

## Build prerequisites

1. **ESP-IDF 6.1, built from source — not the prebuilt archives.** This is the hardest
   prerequisite. The build needs an IDF tree that contains the **esp32s31** target:
   `components/esp_hw_support/port/esp32s31/` must exist and `version.txt` should read `6.1.0`.
   The prebuilt ESP-IDF libraries that PlatformIO ships cannot be used: one of those archives leaves
   PMA entry 7 unmapped, so PSRAM is registered with the heap but unusable and the image does not
   fit. Check this before anything else:

   ```bash
   ls $IDF_PATH/components/esp_hw_support/port/ | grep esp32s31
   cat $IDF_PATH/version.txt
   ```

   If your ESP-IDF 6.1 does not carry that port, this target cannot be built — stop there.

2. **The ESP-IDF Python environment.** `idf.py` is run from
   `<IDF_PYTHON_ENV_PATH>/Scripts/python.exe` on Windows; the convention this tree was verified
   against is `IDF_TOOLS_PATH/python_env/idf6.1_py3.12_env`. Export all four variables in the shell
   you build from — `tools/build.bat` refuses to start without `ESP_IDF_VERSION`, because an unset
   value makes `idf.py` die with `TypeError: expected string or bytes-like object, got 'NoneType'`
   *after* a partial configure, which reads like a broken tree.

   ```bash
   export ESP_IDF_VERSION=6.1.0
   export IDF_PATH='M:/cpc/packages/framework-espidf'        # your from-source IDF 6.1
   export IDF_TOOLS_PATH='M:/cpc'
   export IDF_PYTHON_ENV_PATH='M:/cpc/python_env/idf6.1_py3.12_env'
   ```

   Use native drive-letter paths (`M:/...`), not MSYS paths (`/m/...`): the values are consumed by
   native CMake, not by bash.

3. **Two dependencies are not in this repository — fetch them with `tools/bootstrap.sh`.**
   Both are too large to vendor and are git-ignored by design, so a fresh clone does not have them
   and CMake stops at configure time:

   | Missing from the clone | Goes to | Size | Why |
   |---|---|---|---|
   | arduino-esp32 **4.0.0-RC1** (`variants/esp32s31`) | `idf/components/arduino` | 79 MB | the application and the display driver both `REQUIRES arduino` |
   | **wolfSSL / Arduino-wolfSSL 5.7.2** | `.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL` | 45 MB | TLS for network sync; GPLv2, so it is fetched rather than vendored |

   ```bash
   bash tools/bootstrap.sh
   ```

   It clones both (shallow, one branch each), then verifies the seven libraries that *are* vendored
   (`idf/components/extern/`) are present, and warns if the IDF variables are not exported. It is
   idempotent — re-running it does nothing if both are already there. Upstream has no `4.0.0` tag;
   the release that reports `"version": "4.0.0"` with an ESP32-S31 variant is `4.0.0-RC1`, and
   `master` (3.3.x) has no S31 variant at all, so a stock clone from `master` will not work.

   If you already use PlatformIO, `pio pkg install -e metalio_eink4_plus` fetches the same wolfSSL
   package (plus the rest of `lib_deps`) into the same `.pio/libdeps/` tree — either route satisfies
   the build. You can also point `CROSSPOINT_WOLFSSL_DIR` at any unpacked copy.

4. **MSYS2 / git-bash on Windows** (any bash works), plus `python3`. The build *entry point* is a
   `.bat` because `idf.py` refuses to run from an MSYS shell — it prints
   `MSys/Mingw is no longer supported...` and **exits 0 without building**. A one-line log and no
   binary, reported as success, is the classic false green on this tree.

5. **esptool 5.6.0 or newer** for flashing. It ships an ESP32-S31 stub
   (`targets/stub_flasher/2/esp32s31.json`); earlier versions fall back to the ROM loader.

## Build

```bash
git clone --recursive https://github.com/bartiprime/metalio-crosspoint-public.git
cd metalio-crosspoint-public

# prerequisite 3 — fetches arduino-esp32 4.0.0-RC1 and Arduino-wolfSSL 5.7.2:
bash tools/bootstrap.sh

export ESP_IDF_VERSION=6.1.0
export IDF_PATH='M:/cpc/packages/framework-espidf'
export IDF_TOOLS_PATH='M:/cpc'
export IDF_PYTHON_ENV_PATH='M:/cpc/python_env/idf6.1_py3.12_env'

MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "tools\\build.bat -B build \
  -DCP_ICON_PARTIAL_REPAINT=OFF \
  -DCP_WAKE_SETTLE_EXTRA_MS=800 \
  -DCP_LOG_HOLD_DURING_DRIVE=ON \
  build"
```

Artifact: `build/crosspoint_metalio.bin`. (`-B` is resolved against the repo root, not `idf/`, so
the image lands where you asked for it.)

**Why those three flags.** They are not optional tuning — they are the configuration the device
actually runs, and the CMake defaults differ:

| Flag | Shipping value | Why it matters |
|---|---|---|
| `CP_ICON_PARTIAL_REPAINT` | `OFF` | Selects the battery icon's repaint path. The CMake default is `ON`, so a build that omits it is a different product — and a different battery-drain measurement — that still compiles cleanly. |
| `CP_WAKE_SETTLE_EXTRA_MS` | `800` | Extra settle time after the resume autoflush, before the panel is re-primed. Too short and a wake shows a half-painted screen. |
| `CP_LOG_HOLD_DURING_DRIVE` | `ON` | Holds the log while the panel is being driven, so logging cannot land mid-transaction and stall the drive. |

The canonical list is `tools/shipping_flags.txt`; `tools/check_build_flags.py <build-dir>` compares
a build tree against it and fails on any drift. Run it after any build you intend to flash.

On a non-Windows host, call `idf.py` directly from `idf/` with the same `-D` flags — untested here,
but that is the only difference.

### Tests (host, no device)

```bash
python3 -m pytest test
```

## Troubleshooting a clean build

| Symptom | Cause / fix |
|---|---|
| `CMake Error ... crosspoint_app/CMakeLists.txt:196` → `Arduino-wolfSSL not found at .../.pio/libdeps/metalio_eink4_plus/Arduino-wolfSSL/src` | Prerequisite 3 was skipped. Run `bash tools/bootstrap.sh`. |
| `CMake Error ... Third-party library '<name>' not found` | The clone is incomplete (vendored libraries live in `idf/components/extern/`). Re-clone; do not hand-copy them in. |
| `CMakeLists.txt:... "arduino" component not found` / unknown component `arduino` | `idf/components/arduino` is missing or is a 3.x core. Run `bash tools/bootstrap.sh`. |
| `fatal error: I18nKeys.h: No such file or directory` | The i18n tables are generated and git-ignored; configure regenerates them via `scripts/gen_i18n.py`. A missing `.yaml` set or a Python without the `requirements.txt` packages is the cause. |
| `[build.bat] FATAL: ESP_IDF_VERSION is not set` | Prerequisite 2: export all four IDF variables. |
| `idf.py` prints `MSys/Mingw is no longer supported` and exits 0 with no binary | You called `idf.py` directly from bash. Use `tools/build.bat` through `cmd.exe`. |
| `ESP_ROM_ELF_DIR environment variable is not defined` | A fresh build dir; `tools/build.bat` derives it from `IDF_TOOLS_PATH`. If you call `idf.py` yourself, set it. |
| `submodule update` leaves `freeink-sdk/` empty | Run `git submodule update --init --recursive` again from the repository root. |
| Device appears dead right after a successful flash | Power cycle by hand — see the Flash section. |

## Flash

The image is three regions. `tools/flash.sh` writes exactly these and verifies with the chip's
SPI_FLASH_MD5 (`Hash of data verified` — the exit code alone is not proof, esptool can exit 0 on a
partial write):

```bash
PORT=COM7 tools/flash.sh build/crosspoint_metalio.bin
```

The equivalent esptool invocation, if you would rather see every byte:

```bash
esptool --port COM7 --baud 921600 --before default-reset --after hard-reset \
  write-flash --compress \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/crosspoint_metalio.bin
```

- `0x2000` bootloader, `0x8000` partition table, `0x10000` application.
- `--compress` matters on this chip: the image deflates to ~58 % of its size, and the ROM loader's
  per-block turnaround dominates otherwise. `--no-stub` silently disables compression.
- Nothing here erases NVS or repartitions. Avoid `--erase-all` (wipes settings and calibration).

**Power cycle by hand after flashing.** esptool issues a hard reset, but the SoC does not reliably
come back out of it on this board: switch the device fully off and on again before judging the
image. A device that looks dead straight after a good flash has not booted yet.

## First run

1. **Fonts on the SD card.** Put fonts on the card (`.cpfont`, or `.ttf`/`.otf`/`.ttc` when external
   RAM is enabled) — see [`docs/sd-card-fonts.md`](docs/sd-card-fonts.md). Without one you get the
   built-in font only.
2. **SD card** formatted FAT32/exFAT; books go anywhere the browser can reach.
3. **Calibrate the flick page turn** (Settings → Flick calibration) if you want tilt-to-turn; the
   accelerometer mounting differs per unit, so the shipped orientation is only a starting point.
4. **Set the clock and theme**, then open `Settings → Battery statistics` to confirm the fuel gauge
   is talking to the board before you trust any drain figure.

## Licence and credits

Root licence: **MIT**, `Copyright (c) 2025 CrossPoint Reader organization` — unchanged from
upstream. This fork does not relicense anything.

- **CrossPoint Reader** — the upstream project this is a fork of:
  <https://github.com/crosspoint-reader/crosspoint-reader> (MIT).
- **FreeInk SDK** — the `freeink-sdk` submodule, MIT, © 2026 FreeInk, derived in part from the
  OpenX4 E-Paper Community SDK (MIT); its `NOTICE` carries the upstream text. Public mirror used by
  this repo: <https://github.com/bartiprime/metalio-freeink-sdk-public>.
- **Vendor display driver** — `idf/components/metalio_driver/` is the vendor's EPDiy-derived panel
  and power code for this board, taken from the vendor firmware tree with licence headers stripped.
  Upstream EPDiy is LGPL-3.0; those files are redistributed under LGPL-3.0 with the upstream licence
  text included. See [THIRD_PARTY.md](THIRD_PARTY.md) for the per-directory position.
- **Panel waveforms** — Apache-2.0, © 2026 mindreset, from `wegooo-cell/read-pico-reader`.
- **Image codecs, filesystem, websockets** — BitBank AnimatedGIF/JPEGDEC/PNGdec (Apache-2.0),
  SdFat (MIT), arduinoWebSockets (LGPL-2.1), libb64 (public-domain dedication).

Every third-party component and its licence is listed in [THIRD_PARTY.md](THIRD_PARTY.md). If you
build a binary from this tree, keep those notices with it.
