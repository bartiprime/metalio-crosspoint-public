"""The r163 power-key sleep contract: the six r162 guards must survive the classifier.

Run with python3 test/test_power_sleep_guard_contract.py (requires a host C++ compiler).

r163 B5 replaced the ad-hoc power-key timing in main.cpp with
`powergesture::Classifier`. The classifier is allowed to name the gesture; it is NOT
allowed to change when the device sleeps. This test pins both halves:

  A. STRUCTURAL -- the six guard terms are still present, in one expression, in
     src/main.cpp. A future edit that drops one (or reorders the expression so that the
     classifier verdict can only ever ADD a sleep) fails here on the host, without an ESP
     build. The settings side of guard 5 (`getPowerButtonDuration()`) is pinned to the
     literal it must keep.

  B. SEMANTIC -- the real header is compiled against a harness that reproduces the guard
     expression from main.cpp and proves that the one case the classifier can subtract
     (`Discarded`: a press whose DOWN edge predates this boot) is the only case it
     subtracts, and that every r162 case still sleeps.
"""

from pathlib import Path
import os
import re
import shutil
import subprocess
from scratch_dir import scratch_dir

ROOT = Path(__file__).resolve().parents[1]
main_source = (ROOT / "src/main.cpp").read_text()
settings_source = (ROOT / "src/CrossPointSettings.h").read_text()


def host_compiler():
    for candidate in (os.environ.get("CXX"), "clang++", "g++", "c++"):
        if not candidate:
            continue
        found = shutil.which(candidate)
        if found and subprocess.run([found, "--version"], capture_output=True).returncode == 0:
            return found
    raise SystemExit("test_power_sleep_guard_contract: no C++ compiler found (set CXX)")


# --- A: structural ------------------------------------------------------------------------------
# The decision expression, whitespace-normalised. Both the pre-r163 shape and the r163 shape are
# accepted for the first five terms; what must never go missing is any one of them.
decision = main_source.split("const bool metalioShouldSleep =", 1)[1].split(";", 1)[0]
flat = re.sub(r"\s+", " ", decision).strip()

guards = {
    # 1. A release that woke the device must not sleep it.
    "powerReleasedSinceWake": "powerReleasedSinceWake" in flat,
    # 2. A deliberate long hold still reaches the power-off dialog.
    "metalioHeldMs < METALIO_POWER_OFF_HOLD_MS": "metalioHeldMs < METALIO_POWER_OFF_HOLD_MS" in flat,
    # 3. Nothing before allowSleepAt, judged at the PRESS START.
    "metalioSleepAllowed": "metalioSleepAllowed" in flat,
    # 4. The screenshot combo survives.
    "!gpio.isPressed(HalGPIO::BTN_DOWN)": "!gpio.isPressed(HalGPIO::BTN_DOWN)" in flat,
    # 5. The click threshold itself (10 ms only for SLEEP -- see below).
    "metalioHeldMs > powerDuration": "metalioHeldMs > powerDuration" in flat,
}
for name, present in guards.items():
    assert present, f"r162 guard lost from metalioShouldSleep: {name}"

# The classifier verdict may only be an EXTRA condition, and only after the five above.
assert flat.find("metalioHeldMs > powerDuration") < flat.find("metalioGesture.kind"), (
    "the classifier verdict must not gate the press duration: it can only subtract a sleep"
)
assert "result_of_allowSleep" not in flat  # placeholder-era typo guard
assert "const bool metalioSleepAllowed = metalioPressStartMs >= allowSleepAt;" in main_source, (
    "allowSleepAt must still be judged at the PRESS START, not at release"
)
assert "if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;" in main_source, (
    "powerReleasedSinceWake must still latch on a real release"
)

# 5 (settings side): the 10 ms threshold exists ONLY for SLEEP. PAGE_TURN / FOOTNOTES /
# CREATE_CLIPPING / FORCE_REFRESH keep the 400 ms window.
assert (
    "return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;"
    in settings_source
), "getPowerButtonDuration() must return 10 ms only for SLEEP and 400 ms otherwise"

# The journal line keeps every field it had, keeps shortPwrBtn=%u, and may only append.
index = main_source.index('"power key UP after %lums (input clock %lums): %s held>%lu=%d held<%lu=%d woke=%d "')
journal = main_source[index:index + 1200]
for field in ("power key UP after %lums", "input clock %lums", "%s held>%lu=%d", "held<%lu=%d",
              "woke=%d", "allowSleep=%d", "downKey=%d", "shortPwrBtn=%u"):
    assert field in journal, f"journal field lost: {field}"

# --- B: semantic --------------------------------------------------------------------------------
harness = (
    r"""
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "util/PowerGestureClassifier.h"

using powergesture::Classifier;
using powergesture::Config;
using powergesture::Kind;

// Densities the Metalio awake-UI loop uses.
static constexpr unsigned long kHoldMs = 2200;
static constexpr unsigned long kSettleMs = 100;
static constexpr unsigned long kPowerDurationSleep = 10;   // getPowerButtonDuration() with SLEEP
static constexpr unsigned long kPowerDurationOther = 400;  // ... with PAGE_TURN and friends

Config cfg() {
  Config c;
  c.holdMs = kHoldMs;
  c.releaseSettleMs = kSettleMs;
  c.multiClickMs = 500;
  return c;
}

// The decision expression from src/main.cpp, term for term (the python half of this test pins
// those terms in the source). `verdict` is the classifier's answer for this release.
bool shouldSleep(const unsigned long heldMs, const unsigned long powerDuration,
                 const bool woke, const unsigned long pressStartMs, const unsigned long allowSleepAt,
                 const bool downKey, const Kind kind) {
  const bool metalioSleepAllowed = pressStartMs >= allowSleepAt;
  return heldMs > powerDuration && heldMs < kHoldMs && woke && metalioSleepAllowed && !downKey &&
         (kind == Kind::Click || kind == Kind::MultiClick);
}

int main() {
  // ---- 1. A plain click still sleeps (r162: click-to-sleep must not regress) ----
  {
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(1100, false);
    const auto v = c.poll(1200);
    assert(v.kind == Kind::Click);
    assert(shouldSleep(v.pressMs, kPowerDurationSleep, true, 1000, 0, false, v.kind));
    assert(v.pressMs == 100);
    std::printf("click at %lums sleeps: OK\n", v.pressMs);
  }

  // ---- 2. woke: a release that woke the device must not sleep it ----
  {
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(1100, false);
    const auto v = c.poll(1200);
    assert(!shouldSleep(v.pressMs, kPowerDurationSleep, false, 1000, 3000, false, v.kind));
    std::printf("post-wake release does not sleep: OK\n");
  }

  // ---- 3. allowSleepAt: a press that STARTED inside the window cannot sleep ----
  {
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(1100, false);
    const auto v = c.poll(1200);
    // the press started at 1000, allowSleepAt is 3000
    assert(!shouldSleep(v.pressMs, kPowerDurationSleep, true, 1000, 3000, false, v.kind));
    // ... while the same press, long enough after the window, does.
    assert(shouldSleep(v.pressMs, kPowerDurationSleep, true, 4000, 3000, false, v.kind));
    std::printf("allowSleepAt is judged at the press start: OK\n");
  }

  // ---- 4. the screenshot combo suppresses sleep ----
  {
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(1100, false);
    const auto v = c.poll(1200);
    assert(!shouldSleep(v.pressMs, kPowerDurationSleep, true, 1000, 3000, true, v.kind));
    std::printf("BTN_DOWN suppresses sleep: OK\n");
  }

  // ---- 5. a hold past METALIO_POWER_OFF_HOLD_MS is the dialog's, not sleep's ----
  {
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(3200, false);
    const auto v = c.poll(3300);
    assert(v.kind == Kind::Hold);
    assert(!shouldSleep(v.pressMs, kPowerDurationSleep, true, 1000, 3000, false, v.kind));
    // ... and exactly holdMs-1 is still a click, so the bound is a real boundary.
    Classifier d(cfg());
    d.beginBootMs(0, false);
    d.observeLevel(1000, true);
    d.observeLevel(1000 + kHoldMs - 1, false);
    const auto w = d.poll(1000 + kHoldMs + kSettleMs);
    assert(w.kind == Kind::Click);
    assert(shouldSleep(w.pressMs, kPowerDurationSleep, true, 1000, 0, false, w.kind));
    std::printf("hold >= %lums is the dialog's, %lums is a click: OK\n", kHoldMs, kHoldMs - 1);
  }

  // ---- 6. the 400 ms window is untouched for the other short-press actions ----
  {
    // A 150 ms tap with shortPwrBtn = PAGE_TURN must NOT sleep (400 ms threshold).
    Classifier c(cfg());
    c.beginBootMs(0, false);
    c.observeLevel(1000, true);
    c.observeLevel(1150, false);
    const auto v = c.poll(1250);
    assert(v.kind == Kind::Click);
    assert(!shouldSleep(v.pressMs, kPowerDurationOther, true, 1000, 3000, false, v.kind));
    // 401 ms does.
    Classifier d(cfg());
    d.beginBootMs(0, false);
    d.observeLevel(1000, true);
    d.observeLevel(1401, false);
    const auto w = d.poll(1510);
    assert(shouldSleep(w.pressMs, kPowerDurationOther, true, 1000, 0, false, w.kind));
    std::printf("400 ms window preserved for non-SLEEP modes: OK\n");
  }

  // ---- 7. the one case the classifier subtracts: a press inherited across a restart ----
  {
    // The image restarts with the key held; the new epoch begins at 40 ms.
    Classifier c(cfg());
    c.beginBootMs(40, true);
    assert(c.pressInherited());
    c.observeLevel(900, false);
    const auto v = c.poll(1000);
    assert(v.kind == Kind::Discarded);
    // Even though every r162 guard would have allowed the sleep, the verdict does not.
    const bool sixGuards = v.pressMs > kPowerDurationSleep && v.pressMs < kHoldMs;
    assert(sixGuards);
    assert(!shouldSleep(v.pressMs, kPowerDurationSleep, true, 40, 0, false, v.kind));
    // The next press classifies normally and sleeps.
    c.observeLevel(1200, true);
    c.observeLevel(1300, false);
    const auto next = c.poll(1400);
    assert(next.kind == Kind::Click);
    assert(shouldSleep(next.pressMs, kPowerDurationSleep, true, 1200, 0, false, next.kind));
    std::printf("restart-inherited press is discarded, the next click sleeps: OK\n");
  }

  std::printf("power sleep guard contract: OK\n");
}
"""
)

with scratch_dir("power_sleep_guard_contract") as directory:
    directory = Path(directory)
    compiler = host_compiler()
    cpp = directory / "power_sleep_guards.cpp"
    executable = directory / "power_sleep_guards.exe"
    cpp.write_text(harness)
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", f"-I{ROOT / 'src'}", str(cpp), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("Power-key sleep guard contract checks passed")
