#pragma once

// ---------------------------------------------------------------------------
// STANDBY TRIMS — one predicate for "is this trim active", with a RUNTIME override.
//
// WHY THIS EXISTS (r98.1)
//   CP_CUT_PA_RAIL_ON_STANDBY and CP_SD_PARK_ON_STANDBY are compile-time A/B arms:
//   measuring them that way costs one flash per arm, and the interesting question
//   ("does the P1.4 cut actually save current over 30 min of standby?") needs many
//   runs, interleaved, on the same battery state. So the standby path now asks
//   standbytrim::active() instead of reading the macro directly.
//
//   The macro still decides the DEFAULT: with no override, active() returns exactly
//   the compile-time value, so both-OFF shipping images behave as they always did
//   and an image built with -D...=ON still measures that arm with no command typed.
//   `trim pa on|off` (src/debug/UsbShell.cpp) sets the override for the next
//   standby; nothing about the shutdown/standby ORDER changes, because the call
//   sites in main.cpp run the same code either way.
//
//   NOT a default change: the override lives in RAM and dies with the session. A
//   reboot always returns to the image's build default.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace standbytrim {

enum class Trim : uint8_t { PaRail = 0, SdPark = 1, CamRail = 2 };

// The value compiled into this image (the CP_* macro). Reported by `trim status` so
// a run can never be attributed to a trim that was not even built in.
bool buildDefault(Trim t);

// The value the standby path uses: the runtime override when one is set, otherwise
// buildDefault(t).
bool active(Trim t);

// -1 = no override, 0 = forced off, 1 = forced on.
int8_t overrideValue(Trim t);
void setOverride(Trim t, bool on);
void clearOverride(Trim t);

const char* name(Trim t);      // "pa-rail" | "sd-park" | "cam-rail"
const char* macroName(Trim t); // "CP_CUT_PA_RAIL_ON_STANDBY" | "CP_SD_PARK_ON_STANDBY"
                               // | "CP_CUT_CAM_RAIL_ON_STANDBY"

// True across the standby park (set by main.cpp before the panel/rails are parked,
// cleared once the resume has finished restoring them). `trim` refuses to change a
// trim while this is set: an override applied mid-park would leave the rail in a
// state neither arm describes.
void setParked(bool parked);
bool parked();

// The sleep journal lives in main.cpp as a static variadic function; this is the
// single-line hook into it, installed once from setup(). Until it is installed,
// journal() is a no-op (there is nowhere truthful to write).
void setJournal(void (*fn)(const char*));
void journal(const char* line);

}  // namespace standbytrim
