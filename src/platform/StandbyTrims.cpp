#include "platform/StandbyTrims.h"

#include "display/RefreshPolicy.h"  // the #ifndef defaults for both CP_* switches

namespace standbytrim {
namespace {

// -1 = follow the build default. Reset on every boot by static initialisation: the
// override is a measurement aid for one session, never a stored setting.
int8_t g_override[3] = {-1, -1, -1};

bool g_parked = false;
void (*g_journal)(const char*) = nullptr;

int idx(const Trim t) { return static_cast<int>(t); }

}  // namespace

bool buildDefault(const Trim t) {
  switch (t) {
    case Trim::PaRail:
      return CP_CUT_PA_RAIL_ON_STANDBY != 0;
    case Trim::SdPark:
      return CP_SD_PARK_ON_STANDBY != 0;
    case Trim::CamRail:
      return CP_CUT_CAM_RAIL_ON_STANDBY != 0;
  }
  return false;
}

bool active(const Trim t) {
  const int8_t o = g_override[idx(t)];
  if (o < 0) return buildDefault(t);
  return o != 0;
}

int8_t overrideValue(const Trim t) { return g_override[idx(t)]; }

void setOverride(const Trim t, const bool on) { g_override[idx(t)] = on ? 1 : 0; }

void clearOverride(const Trim t) { g_override[idx(t)] = -1; }

const char* name(const Trim t) {
  switch (t) {
    case Trim::PaRail:
      return "pa-rail";
    case Trim::SdPark:
      return "sd-park";
    case Trim::CamRail:
      return "cam-rail";
  }
  return "?";
}

const char* macroName(const Trim t) {
  switch (t) {
    case Trim::PaRail:
      return "CP_CUT_PA_RAIL_ON_STANDBY";
    case Trim::SdPark:
      return "CP_SD_PARK_ON_STANDBY";
    case Trim::CamRail:
      return "CP_CUT_CAM_RAIL_ON_STANDBY";
  }
  return "?";
}

void setParked(const bool parked) { g_parked = parked; }
bool parked() { return g_parked; }

void setJournal(void (*fn)(const char*)) { g_journal = fn; }

void journal(const char* line) {
  if (g_journal != nullptr && line != nullptr) g_journal(line);
}

}  // namespace standbytrim
