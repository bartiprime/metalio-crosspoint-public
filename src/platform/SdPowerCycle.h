#pragma once

// ---- r109 CARD1: settle dwells around an SD card power cycle ------------------------------
// WHY: every park/resume (and every `sd umount` + `sd mount`) power-cycles the card twice:
// the volume ends and the SDMMC host is deinitialised, then the CNNT pads are released; on
// the way back the pads are re-claimed and the slot driver re-runs the OEM's timed HIGH->LOW
// power cycle. Both edges used to happen back-to-back with no dwell. A card that has just
// taken its last write needs the bus to stay up while it finishes its own internal
// housekeeping, and a card whose supply has just returned needs a settle before the first
// CMD0. On 10 Nov 2026 a card that came back from exactly this sequence had lost its
// directory tree, and the firmware then wrote defaults over the clusters -- so the two
// edges are now separated by a minimum dwell in BOTH directions.
//
// The values are floors, and what each call actually waited is logged: one INFO line per
// cycle, so a glass log carries the numbers the hardware really used.

#include <Arduino.h>
#include <Logging.h>

namespace sdpower {

// >= 150 ms with the bus still up after vol.end()/_dev->end(), before the pads go high-Z.
inline constexpr uint32_t kSettleAfterLastWriteMs = 150;
// >= 50 ms after the pads are re-claimed, before the first read of a mount attempt.
inline constexpr uint32_t kSettleBeforeMountMs = 50;

// Call AFTER the volume/host have been ended and BEFORE the CNNT pad release.
inline uint32_t dwellAfterLastWrite(const char* where) {
  const uint32_t t0 = millis();
  delay(kSettleAfterLastWriteMs);
  const uint32_t used = millis() - t0;
  LOG_INF("SD", "card power-cycle %s: settle after last write/end() = %lu ms (bus kept up for card housekeeping)",
          where, static_cast<unsigned long>(used));
  return used;
}

// Call AFTER the pads are re-claimed and BEFORE the mount attempt.
inline uint32_t dwellBeforeMount(const char* where) {
  const uint32_t t0 = millis();
  delay(kSettleBeforeMountMs);
  const uint32_t used = millis() - t0;
  LOG_INF("SD", "card power-cycle %s: settle before first read/mount = %lu ms (supply settled after re-claim)",
          where, static_cast<unsigned long>(used));
  return used;
}

}  // namespace sdpower
