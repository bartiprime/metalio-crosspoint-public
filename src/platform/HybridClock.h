#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// HYBRID CLOCK — Build A step 3 (item C), docs/engineering/HARDWARE_POWER_PLAN.md §5.1/§5.2.
//
// WHY THIS EXISTS
//   This board has two clocks and neither is the whole answer:
//
//     esp_timer / millis()  XTAL-backed. Accurate while the CPU runs. In light sleep the
//                           counter is not clocked; on resume the IDF advances it by the
//                           RTC-measured sleep length, so a delta across a park is the RTC
//                           number wearing an esp_timer name.
//     RTC counter           the internal RC oscillator (~0.5-1 % after calibration,
//                           sdkconfig CONFIG_RTC_CLK_SRC_INT_RC). It keeps counting while
//                           the CPU is parked, and it is the ONLY clock that does. It is
//                           also what gettimeofday() advances by
//                           (CONFIG_NEWLIB_TIME_SYSCALL_USE_RTC_HRT), i.e. the epoch is
//                           RC-backed too -- the CALENDAR is the PCF8563, a different path.
//
//   So: elapsed time is measured with esp_timer while awake, with the RTC tick delta while
//   parked, and the parked span carries a STORED drift factor (ppm). The factor starts at 0
//   -- uncorrected, the honest default -- and is set from a measurement (host wall clock vs
//   this number over one 8 h park, §8.2), never invented here.
//
//   A mA figure derived from these numbers is PROVISIONAL: the denominator rides the RC
//   oscillator, which is the thing under test. Charge comes from the gauge's coulomb
//   counter (0x10, 1 mAh LSB); elapsed comes from the HOST wall clock.
//
//   Never silently substitute one clock for the other: every switch of source logs at DEBUG.
//
//   No I2C. Nothing here is on the awake hot path: the park mark is taken from the coulomb
//   read wakerec already performed, so the park path makes exactly the same bus traffic.
// ---------------------------------------------------------------------------

namespace hybridclock {

// Source names, for a log line that has to say WHICH clock a number came from.
const char* awakeSource();  // "esp_timer (XTAL)"
const char* parkedSource(); // "rtc counter (internal RC)"

uint64_t awakeUs();        // esp_timer_get_time()
uint64_t rtcUs();          // esp_rtc_get_time_us(): the RC tick count, keeps counting in light sleep
unsigned long uptimeMs();  // the SoC uptime `ver`/`batt` report (esp_timer-backed)

// Stored drift factor, parts per million, applied to the PARKED span only.
//   0    = uncorrected (the compiled default: no measurement has been entered yet)
//   +50  = the RTC runs 50 ppm fast -> a 1000 s raw park is corrected to 1000.05 s
// Persisted in NVS (namespace "cpclk", key "ppm"), loaded lazily on first use.
int32_t driftPpm();
// Clamped to +-20000 ppm; persists. Returns the value actually stored.
int32_t setDriftPpm(int32_t ppm);
// True when the factor has been loaded/entered this session (false = never touched NVS yet).
bool driftLoaded();

// raw parked microseconds -> corrected, using the stored factor.
uint64_t correctParkedUs(uint64_t rawUs);

// One clock reading plus the gauge value taken at the same moment.
struct Mark {
  bool valid = false;
  bool coulombOk = false;
  uint16_t coulombMah = 0;
  int64_t epoch = 0;          // gettimeofday(): RC-backed, NOT the PCF8563 calendar
  unsigned long uptimeMs = 0; // SoC uptime at the mark
  uint64_t rtcUs = 0;         // RTC counter at the mark
  uint64_t espUs = 0;         // esp_timer at the mark
};

// The last park->resume pair, kept so `batt delta` can print it without a second park.
struct Segment {
  bool valid = false;
  uint64_t parkedRawUs = 0;       // RTC delta across the park
  uint64_t parkedCorrectedUs = 0; // same, with the stored ppm applied
  uint64_t espAcrossUs = 0;       // esp_timer delta across the same window (comparison only)
  int32_t ppm = 0;                // factor that was applied
  bool bothOk = false;            // both coulomb reads succeeded
  uint16_t parkMah = 0;
  uint16_t resumeMah = 0;
};

// Take the park half. `mah`/`ok` are the coulomb read the caller already made -- this
// function performs no bus traffic. Logs the DEBUG source switch.
void markPark(uint16_t mah, bool ok);
// Take the resume half, compute the segment, log the DEBUG source switch back to esp_timer.
void markResume(uint16_t mah, bool ok);
// r106 B4: the IDLE mark. Same shape and the same single slot as the park mark, taken
// while the device is AWAKE, so `batt delta` brackets an awake interval (A9 had no
// instrument for that). RAM only, no NVS. `what` is the label the caller printed.
void markIdle(uint16_t mah, bool ok, const char* what);

const Mark& parkMark();
const Mark& resumeMark();
const Segment& lastSegment();

// Device-time elapsed from the last park mark to NOW: parked (RTC, corrected) + awake
// (esp_timer since the resume). 0 when there is no mark (a reboot between the two).
uint64_t sinceMarkUs();

// One-line label for the factor, e.g. "0 ppm (uncorrected)" or "+120 ppm".
const char* driftLabel(char* out, unsigned cap);

}  // namespace hybridclock
