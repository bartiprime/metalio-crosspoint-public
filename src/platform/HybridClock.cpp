// HYBRID CLOCK — see src/platform/HybridClock.h for the contract and the reasoning.
//
// Implementation notes that matter to a reader of the numbers:
//   * rtcUs() is esp_rtc_get_time_us(): the RTC counter converted with the slow-clock
//     calibration the IDF itself keeps. It is the SAME counter that backs light sleep and
//     the epoch, which is why the parked span and the epoch both carry RC drift.
//   * The drift factor corrects the parked span only. Correcting the awake span would be
//     wrong (esp_timer is XTAL) and correcting the epoch here would double-correct, because
//     gettimeofday already consumes the same RTC ticks.
//   * A mark that cannot fill its RTC counter (0, or a counter that did not advance across
//     the park) falls back to esp_timer and says so at DEBUG — a substitution is never
//     silent.

#include "platform/HybridClock.h"

#include <Preferences.h>
#include <esp_rtc_time.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace hybridclock {
namespace {

// NVS: one int32. 32-bit Preferences "Long" is int32_t on this port, which is what a ppm
// factor needs (a 32.768 kHz RC part is never 2^31 ppm out).
constexpr const char* NVS_NAMESPACE = "cpclk";
constexpr const char* NVS_KEY = "ppm";
constexpr int32_t kPpmLimit = 20000;  // 2 %: beyond this the part is broken, not miscalibrated

int32_t g_ppm = 0;
bool g_ppmLoaded = false;

Mark g_park;
Mark g_resume;
Segment g_segment;

uint64_t nowEpoch() {
  struct timeval tv{};
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return static_cast<uint64_t>(tv.tv_sec);
}

void loadPpmOnce() {
  if (g_ppmLoaded) return;
  g_ppmLoaded = true;
  // r107 P8: opened READ-WRITE, deliberately. A READ-ONLY open of a namespace that does not
  // exist yet fails with ESP_ERR_NVS_NOT_FOUND and Preferences prints
  // `nvs_open failed: NOT_FOUND`, which on a cold device is the FIRST `batt mark`/`batt delta`
  // line -- pure noise about an absent key, not a fault. A read-write open creates the empty
  // namespace (a few bytes, once per device) and answers the same default 0, so the honest
  // default still wins and the console stops reporting a non-event. No other NVS path changes.
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  const int32_t v = prefs.getInt(NVS_KEY, 0);
  prefs.end();
  g_ppm = v < -kPpmLimit ? -kPpmLimit : (v > kPpmLimit ? kPpmLimit : v);
}

}  // namespace

const char* awakeSource() { return "esp_timer (XTAL)"; }
const char* parkedSource() { return "rtc counter (internal RC)"; }

uint64_t awakeUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
uint64_t rtcUs() { return esp_rtc_get_time_us(); }
unsigned long uptimeMs() { return static_cast<unsigned long>(esp_timer_get_time() / 1000LL); }

bool driftLoaded() { return g_ppmLoaded; }

int32_t driftPpm() {
  loadPpmOnce();
  return g_ppm;
}

int32_t setDriftPpm(const int32_t ppm) {
  loadPpmOnce();
  const int32_t v = ppm < -kPpmLimit ? -kPpmLimit : (ppm > kPpmLimit ? kPpmLimit : ppm);
  g_ppm = v;
  g_ppmLoaded = true;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putInt(NVS_KEY, v);
    prefs.end();
    LOG_INF("PWR", "clock: drift factor stored ppm=%ld (applied to the parked span only)", static_cast<long>(v));
  } else {
    LOG_ERR("PWR", "clock: drift factor ppm=%ld NOT persisted (NVS open failed) — RAM only this session",
            static_cast<long>(v));
  }
  return v;
}

uint64_t correctParkedUs(const uint64_t rawUs) {
  loadPpmOnce();
  if (g_ppm == 0) return rawUs;
  // raw * (1e6 + ppm) / 1e6, kept in 64 bits: raw is at most 2^48 and the factor at most 1.02.
  return static_cast<uint64_t>((static_cast<long long>(rawUs) * (1000000LL + g_ppm)) / 1000000LL);
}

void markPark(const uint16_t mah, const bool ok) {
  loadPpmOnce();
  Mark m;
  m.valid = true;
  m.coulombOk = ok;
  m.coulombMah = mah;
  m.epoch = static_cast<int64_t>(nowEpoch());
  m.uptimeMs = uptimeMs();
  m.rtcUs = rtcUs();
  m.espUs = awakeUs();
  g_park = m;
  g_resume = Mark{};
  g_segment = Segment{};
  // The substitution this module exists to make explicit. DEBUG, not INFO: it fires twice a
  // park and says nothing about the device's behaviour, only which counter is carrying time.
  LOG_DBG("PWR",
          "clock: awake span was %s; parked span now rides %s (ppm=%ld)%s — gettimeofday keeps advancing on the "
          "same RC ticks",
          awakeSource(), parkedSource(), static_cast<long>(g_ppm),
          g_ppm == 0 ? " (UNcorrected)" : "");
}

void markResume(const uint16_t mah, const bool ok) {
  loadPpmOnce();
  Mark m;
  m.valid = true;
  m.coulombOk = ok;
  m.coulombMah = mah;
  m.epoch = static_cast<int64_t>(nowEpoch());
  m.uptimeMs = uptimeMs();
  m.rtcUs = rtcUs();
  m.espUs = awakeUs();
  g_resume = m;

  Segment s;
  if (!g_park.valid) {
    // A reboot between the two: no park half, so no delta and no drift measurement. Say it
    // rather than inventing a span from boot time.
    LOG_DBG("PWR", "clock: resume mark with NO park mark this session — parked span unknown, clock source stays %s",
            awakeSource());
    g_segment = s;
    return;
  }

  s.valid = true;
  s.ppm = g_ppm;
  s.parkMah = g_park.coulombMah;
  s.resumeMah = m.coulombMah;
  s.bothOk = g_park.coulombOk && m.coulombOk;
  s.espAcrossUs = (m.espUs > g_park.espUs) ? (m.espUs - g_park.espUs) : 0;

  // The RTC counter is the parked-span source. A counter that did not advance -- or one that
  // read 0 at the park, which is what esp_rtc_get_time_us() returns before the slow-clock
  // calibration exists -- is not a measurement: fall back to esp_timer's own cross-park delta
  // and SAY so.
  const bool rtcAdvanced = g_park.rtcUs != 0 && m.rtcUs > g_park.rtcUs;
  if (rtcAdvanced) {
    s.parkedRawUs = m.rtcUs - g_park.rtcUs;
    s.parkedCorrectedUs = correctParkedUs(s.parkedRawUs);
  } else {
    s.parkedRawUs = 0;
    s.parkedCorrectedUs = s.espAcrossUs;
    // Always-on (ERR): with the runtime debug gate off, INF/DBG are silent, and without this line a
    // fallback park reads as a bare "parked 0 ms" with no explanation. ERR is the only level that
    // reaches the shipping default console + crash ring.
    LOG_ERR("PWR",
            "clock: RTC FALLBACK - the RTC counter did not advance across the park (park read=%llu us, "
            "resume read=%llu us), so the parked span is substituted from esp_timer (XTAL): "
            "corrected=%llu ms (RTC raw=0), awake span back on %s (esp delta across the park=%llu ms)",
            static_cast<unsigned long long>(g_park.rtcUs), static_cast<unsigned long long>(m.rtcUs),
            static_cast<unsigned long long>(s.parkedCorrectedUs / 1000), awakeSource(),
            static_cast<unsigned long long>(s.espAcrossUs / 1000));
  }
  g_segment = s;

  LOG_DBG("PWR",
          "clock: parked span from %s raw=%llu ms corrected=%llu ms (ppm=%ld)%s; awake span back on %s (esp delta "
          "across the park=%llu ms)",
          rtcAdvanced ? parkedSource() : "esp_timer (XTAL) — the RTC counter did not advance",
          static_cast<unsigned long long>(s.parkedRawUs / 1000),
          static_cast<unsigned long long>(s.parkedCorrectedUs / 1000),
          static_cast<long>(s.ppm), rtcAdvanced ? "" : " [RTC FALLBACK]", awakeSource(),
          static_cast<unsigned long long>(s.espAcrossUs / 1000));
}

const Mark& parkMark() { return g_park; }
const Mark& resumeMark() { return g_resume; }
const Segment& lastSegment() { return g_segment; }

// r106 B4. One mark slot, deliberately: an idle mark REPLACES the park mark, because
// `batt delta` can only mean one interval at a time. The resume half and the parked
// segment are cleared, so sinceMarkUs() falls back to the esp_timer delta -- which is
// exactly right for an AWAKE interval (the RTC counter is only the parked-span clock).
void markIdle(const uint16_t mah, const bool ok, const char* what) {
  loadPpmOnce();
  Mark m;
  m.valid = true;
  m.coulombOk = ok;
  m.coulombMah = mah;
  m.epoch = static_cast<int64_t>(nowEpoch());
  m.uptimeMs = uptimeMs();
  m.rtcUs = rtcUs();
  m.espUs = awakeUs();
  g_park = m;
  g_resume = Mark{};
  g_segment = Segment{};
  LOG_INF("PWR",
          "idle mark: coulomb=%s%u mAh epoch=%lld uptime=%lu ms rtc=%llu us (%s) — awake span, esp_timer",
          ok ? "" : "FAIL ", static_cast<unsigned>(mah), static_cast<long long>(m.epoch), m.uptimeMs,
          static_cast<unsigned long long>(m.rtcUs), what != nullptr ? what : "idle");
}

uint64_t sinceMarkUs() {
  if (!g_park.valid) return 0;
  const uint64_t espNow = awakeUs();
  if (g_segment.valid) {
    const uint64_t awakeSince = (espNow > g_resume.espUs) ? (espNow - g_resume.espUs) : 0;
    return g_segment.parkedCorrectedUs + awakeSince;
  }
  // Marked a park but no resume recorded: only the awake half exists. DEBUG because this is
  // esp_timer answering for a span the RTC counter owns whenever a park really happened.
  LOG_DBG("PWR", "clock: elapsed since mark uses %s only (no resume half recorded)", awakeSource());
  return (espNow > g_park.espUs) ? (espNow - g_park.espUs) : 0;
}

const char* driftLabel(char* out, const unsigned cap) {
  const int32_t v = driftPpm();
  if (out == nullptr || cap == 0) return "";
  if (v == 0) {
    snprintf(out, cap, "0 ppm (uncorrected)");
  } else {
    snprintf(out, cap, "%+ld ppm", static_cast<long>(v));
  }
  return out;
}

}  // namespace hybridclock
