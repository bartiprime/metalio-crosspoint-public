#include "HalClock.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <time.h>

#include <sys/time.h>

#include <cctype>

HalClock halClock;  // Singleton instance

namespace {
// RTC1: the PCF8563's seconds register (0x02) carries the VL flag in bit 7. The
// chip raises VL when its backup supply could not hold the oscillator, so the
// calendar it serves is the last value it knew rather than a record of elapsed
// time. The SDK driver already REFUSES such a read (Rtc.cpp:150, `if (raw[0] &
// PCF8563_VL_FLAG) return false;`), which collapses "chip present, time
// untrusted" and "no chip" into one silent false -- that is what this bit is
// read separately for.
constexpr uint8_t kPcf8563SecondsReg = 0x02;
constexpr uint8_t kPcf8563VlBit = 0x80;
}  // namespace

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  // First VL read of the boot, before anything asks for a time: main.cpp's seed
  // step consults voltageLow() straight after this call.
  bool known = false;
  const bool vl = readVoltageLow(known);
  if (known) noteVoltageLow(vl);
}

bool HalClock::readVoltageLow(bool& known) const {
  known = false;
  const auto& sensors = BoardConfig::ACTIVE.sensors;
  // VL is a PCF8563 flag; the other chips this SDK supports use different
  // registers for their stopped-oscillator condition and are not read here.
  if (sensors.rtcType != BoardConfig::RtcType::Pcf8563 || sensors.rtcAddr == 0) return false;
  const uint8_t addr = sensors.rtcAddr;
  Wire.beginTransmission(addr);
  Wire.write(kPcf8563SecondsReg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(addr), 1) != 1) return false;
  const uint8_t seconds = static_cast<uint8_t>(Wire.read());
  known = true;
  return (seconds & kPcf8563VlBit) != 0;
}

void HalClock::noteVoltageLow(const bool vl) const {
  const bool first = !_vlKnown;
  const bool changed = first || _vl != vl;
  _vl = vl;
  _vlKnown = true;
  if (!changed) return;
  if (vl) {
    // Loud, because this is the one state where the calendar on the chip is a
    // lie: saying nothing would let a stale date look like a fact.
    LOG_ERR("CLK", "RTC voltage-low flag set (vl=1): backup supply lost, calendar NOT trusted");
  } else if (!first) {
    LOG_INF("CLK", "RTC voltage-low flag cleared (vl=0): calendar trusted again");
  }
}

namespace {
// SNTP server set. Literals carry no DNS dependency: on a fake-IP proxy network the resolver hands
// out unroutable 198.18.x addresses, but UDP/123 to a literal IP may still traverse.
constexpr const char* kNtpHost = "pool.ntp.org";
constexpr const char* kNtpFallbackIp = "162.159.200.1";  // time.cloudflare.com, no DNS needed
constexpr const char* kNtpAltIp1 = "216.239.35.0";       // time.google.com, anycast
constexpr const char* kNtpAltIp2 = "129.6.15.28";        // time-a-g.nist.gov
// Probed first on fake-IP networks: Alibaba anycast, answered in 60 ms from this LAN's own host probe.
constexpr const char* kNtpProbeIp = "203.107.6.88";
}  // namespace

namespace {
// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}

// Plausibility window for a time we did not get from SNTP: an HTTP Date header can come from a
// captive portal or a proxy's error page, and a nonsense value must not be written to the RTC.
// 2024-01-01 .. 2100-01-01 UTC, matching the window TrustedTime trusts.
constexpr time_t kMinPlausibleEpoch = 1704067200;
constexpr time_t kMaxPlausibleEpoch = 4102444800;

// Reads up to maxDigits decimal digits, advancing *p. Returns -1 when no digit is present.
int readDigits(const char*& p, int maxDigits) {
  int value = 0;
  int used = 0;
  while (used < maxDigits && *p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    ++p;
    ++used;
  }
  return used > 0 ? value : -1;
}

// "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 1123 IMF-fixdate) -> Unix epoch. Hand-rolled: no allocation,
// no timegm (newlib has none), and it reuses the same days-from-civil code as the RTC reads.
bool epochFromRfc1123(const char* s, time_t& out) {
  if (s == nullptr || *s == '\0') return false;
  static const char* const kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  while (*s != '\0' && *s != ',') ++s;  // skip the (unused) weekday
  if (*s == ',') ++s;
  while (*s == ' ') ++s;

  const int day = readDigits(s, 2);
  if (day < 1 || day > 31 || *s != ' ') return false;
  ++s;

  int month = 0;
  for (int i = 0; i < 12 && month == 0; ++i) {
    bool match = true;
    for (int c = 0; c < 3 && match; ++c) {
      const char a = static_cast<char>(toupper(static_cast<unsigned char>(s[c])));
      match = a != '\0' && a == kMonths[i][c];
    }
    if (match) month = i + 1;
  }
  if (month == 0) return false;
  s += 3;
  if (*s != ' ') return false;
  ++s;

  const int year = readDigits(s, 4);
  if (year < 1970 || *s != ' ') return false;
  ++s;

  const int hour = readDigits(s, 2);
  if (hour < 0 || hour > 23 || *s != ':') return false;
  ++s;
  const int minute = readDigits(s, 2);
  if (minute < 0 || minute > 59 || *s != ':') return false;
  ++s;
  const int second = readDigits(s, 2);
  if (second < 0 || second > 60) return false;  // 60 = leap second; clamp below

  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(year);
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);
  dt.hour = static_cast<uint8_t>(hour);
  dt.minute = static_cast<uint8_t>(minute);
  dt.second = static_cast<uint8_t>(second > 59 ? 59 : second);
  out = epochFromUtc(dt);
  return true;
}
}  // namespace

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    // RTC1: read the VL bit on EVERY clock read, before the time itself. With
    // VL set the SDK's now() refuses and returns false, so this bit is the only
    // way to tell "the chip's backup supply failed" from "there is no chip".
    bool vlKnown = false;
    const bool vl = readVoltageLow(vlKnown);
    if (vlKnown) noteVoltageLow(vl);
    Rtc::DateTime dt;
    if (_sdkRtc.now(dt)) {
      _cachedUtc = epochFromUtc(dt);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
  }
  localtime_r(&_cachedUtc, &out);
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

// Writes the RTC from the SNTP result if the exchange has completed. Returns false while it is still
// in flight, so callers can decide whether to keep waiting.
void HalClock::disableAlarm() {
  if (!_available) return;
  constexpr uint8_t kAddr = 0x51;      // PCF8563
  constexpr uint8_t kControl2 = 0x01;  // control_status_2: AIE is bit 1
  Wire.beginTransmission(kAddr);
  Wire.write(kControl2);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(static_cast<int>(kAddr), 1) != 1) return;
  const uint8_t value = static_cast<uint8_t>(Wire.read());
  Wire.beginTransmission(kAddr);
  Wire.write(kControl2);
  Wire.write(static_cast<uint8_t>(value & ~0x02));  // clear AIE, leave the rest alone
  Wire.endTransmission();
}

bool HalClock::writeRtcIfSynced() {
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) return false;
  return writeRtcFromSystemTime();
}

// The RTC write itself, with no opinion on where the system clock came from. The HTTP-Date
// fallback never sets the SNTP status, so it has to skip writeRtcIfSynced()'s gate and land here.
bool HalClock::writeRtcFromSystemTime() {
  const time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
  dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
  dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
  dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
  dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
  dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
  dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);

  const bool ok = _sdkRtc.set(dt);
  if (ok) {
    _cachedUtc = epochFromUtc(dt);
    _hasCachedTime = true;
    _lastPollMs = 0;
    LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
            dt.second);
  } else {
    // Previously silent: a SNTP success with a failing RTC write looked identical to no sync at all.
    static bool loggedWriteFailure = false;
    if (!loggedWriteFailure) {
      loggedWriteFailure = true;
      LOG_ERR("CLK", "time source reported success but the RTC write failed (PCF8563 @0x51)");
    }
  }
  return ok;
}

// One bounded literal-IP SNTP attempt. Used on fake-IP networks: DNS there cannot return a routable
// address, but UDP/123 to a literal IP may still traverse the same proxy (a host-side probe against
// these destinations answered in 60 ms). A real NTP reply beats an HTTP Date on latency and accuracy.
// All three literals are handed to lwIP so they are polled in parallel and the first reply wins.
bool HalClock::probeLiteralNtp(const char* server) {
  configTzTime("UTC0", server, kNtpAltIp1, kNtpAltIp2);
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  _syncPending = true;
  _syncStartedMs = millis();
  const unsigned long t0 = millis();
  bool ok = false;
  while (millis() - t0 < HTTP_LITERAL_PROBE_MS) {
    if (writeRtcIfSynced()) {
      ok = true;
      break;
    }
    delay(50);
  }
  // This probe owns the pending flag: a reply that lands later must not be applied behind the HTTP
  // fallback's back, and the display timezone configTzTime clobbered has to come back either way.
  _syncPending = false;
  setTimezone(_savedTz);
  LOG_INF("CLK", "literal NTP probe %s (+2 alternates): %s in %lums", server, ok ? "reply" : "no reply",
          millis() - t0);
  return ok;
}

// Sets the clock from an RFC 1123 Date header on a plain-HTTP 204 endpoint. Port 80 and no TLS on
// purpose: a TLS handshake needs a CA clock-check to pass, and this path exists exactly for boards
// whose clock is not set yet. The proxy that breaks SNTP forwards this fine.
bool HalClock::syncFromHttpDate() {
  // Endpoint order: the domestic (CN) 204 endpoints first, as requested, then gstatic. All three are
  // tried within one total budget rather than a single attempt with a second only on a fast failure.
  static constexpr const char* kUrls[] = {"http://connect.rom.miui.com/generate_204",
                                          "http://wifi.vivo.com.cn/generate_204",
                                          "http://connectivitycheck.gstatic.com/generate_204",
                                          "http://cp.cloudflare.com/generate_204"};
  const unsigned long started = millis();
  for (size_t i = 0; i < sizeof(kUrls) / sizeof(kUrls[0]); ++i) {
    const unsigned long elapsed = millis() - started;
    if (i > 0 && elapsed >= HTTP_TOTAL_BUDGET_MS) {
      LOG_INF("CLK", "HTTP time fallback: %lums budget spent, %u host(s) untried", elapsed,
              static_cast<unsigned>(sizeof(kUrls) / sizeof(kUrls[0]) - i));
      break;
    }

    freeink::SecureHttpClient http;
    http.setReuse(false);  // one-shot: a kept-alive socket would outlive this call
    http.setTimeout(HTTP_TIME_TIMEOUT_MS);
    http.setUserAgent("CrossPoint-time-sync");
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("Connection", "close");
    if (!http.begin(kUrls[i])) {
      LOG_ERR("CLK", "HTTP time fallback: connect failed for %s", kUrls[i]);
      continue;
    }

    const unsigned long phaseStart = millis();
    const int status = http.GET([](const uint8_t*, size_t) { return true; });
    const unsigned long phaseMs = millis() - phaseStart;
    if (status <= 0) {
      LOG_ERR("CLK", "HTTP time fallback: no answer from %s after %lums (err=%d)", kUrls[i], phaseMs, status);
      continue;
    }
    LOG_DBG("CLK", "HTTP time fallback: %s answered %d in %lums", kUrls[i], status, phaseMs);
    // Response headers are parsed before the body, so they are readable even on a 204.
    const std::string* date = http.getUniqueHeader("date");
    if (date == nullptr) {
      LOG_ERR("CLK", "HTTP time fallback: %s answered %d in %lums without a Date header", kUrls[i], status,
              phaseMs);
      // Dump what did come back: without this a missing Date is indistinguishable from a proxy
      // interposing a captive-portal page, which is exactly the case this path exists for.
      const auto headers = http.getHeaders();
      for (const auto& h : headers) {
        LOG_DBG("CLK", "  hdr %s: %s", h.first.c_str(), h.second.c_str());
      }
      continue;
    }

    time_t epoch = 0;
    if (!epochFromRfc1123(date->c_str(), epoch)) {
      LOG_ERR("CLK", "HTTP time fallback: unparseable Date header \"%s\"", date->c_str());
      continue;
    }
    if (epoch < kMinPlausibleEpoch || epoch >= kMaxPlausibleEpoch) {
      LOG_ERR("CLK", "HTTP time fallback: implausible Date header \"%s\"; clock left alone", date->c_str());
      continue;
    }

    timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
    LOG_INF("CLK", "clock set from HTTP Date: %s", date->c_str());
    // SNTP persists the TrustedTime floor from its sync callback; this path has no callback, so do
    // it here. note() only ever raises the floor, so it cannot rewind a monotonic basis.
    trustedtime::note();
    // A failing RTC write no longer means the time is lost: the system clock is already set, and
    // writeRtcFromSystemTime() logs the one-shot hardware error itself.
    writeRtcFromSystemTime();
    return true;
  }
  return false;
}

bool HalClock::syncFromNTP() {
  // First thing, before any early return: the ONLY silent failure in this function is `!_available`,
  // and a sync that never ran is otherwise indistinguishable from one that ran and was refused.
  LOG_INF("CLK", "sync attempt: available=%d wifi=%d rssi=%d", static_cast<int>(_available),
          static_cast<int>(WiFi.status()), static_cast<int>(WiFi.RSSI()));
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // configTzTime overwrites the process TZ with UTC0 for the SNTP exchange; remember the display
  // timezone so it can be restored below. Kept in a member because the reply may arrive after this
  // call returns.
  const char* tzBefore = getenv("TZ");
  _savedTz[0] = '\0';
  if (tzBefore) snprintf(_savedTz, sizeof(_savedTz), "%s", tzBefore);
  // Resolve first. configTzTime() gives the SNTP client a hostname, and DNS is least reliable
  // exactly when we need it -- right after DHCP. A dead resolver then burns the whole attempt with
  // no useful error. Resolve explicitly, log the address, and fall back to a literal IP so a broken
  // resolver cannot disable time sync on its own.
  char server[40];
  bool resolved = false;
  for (int attempt = 1; attempt <= 3 && !resolved; ++attempt) {
    IPAddress ip;
    if (WiFi.hostByName(kNtpHost, ip)) {
      snprintf(server, sizeof(server), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      LOG_INF("CLK", "resolved %s -> %s", kNtpHost, server);
      // 198.18.0.0/15 is RFC 2544 benchmark space: no real host answers there. A proxy that hands out
      // fake-IPs from it makes SNTP wait out the whole abandon window and then fail anyway, which is
      // what left the clock screen on "Syncing..." for 30 s. Pointing the SNTP client at 198.18.x is
      // useless either way -- no NTP server can live there -- so SNTP is skipped outright instead of
      // burning the inline window, and the clock comes from an HTTP Date header: TCP/80 does
      // traverse the same proxy.
      if (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19)) {
        LOG_INF("CLK", "NTP host resolved to a fake-IP proxy address (%s); UDP/123 cannot be assumed here",
                server);
        // No SNTP exchange is started on this path, but an earlier one may still be pending (and TZ
        // saved with it): clear both so no late status or stale display timezone survives.
        _httpFallbackDue = false;
        _syncPending = false;
        setTimezone(_savedTz);
        // Literal IPs first: DNS is lying, not the network. 300 ms, and a real reply is both faster
        // and more accurate than an HTTP Date. Falls through to HTTP if nothing answers.
        if (probeLiteralNtp(kNtpProbeIp)) {
          LOG_INF("CLK", "clock set by literal-IP SNTP on a fake-IP network");
          return true;
        }
        if (syncFromHttpDate()) {
          LOG_INF("CLK", "clock set over HTTP; NTP skipped on this network");
          return true;
        }
        LOG_ERR("CLK", "HTTP Date fallback failed on a fake-IP network; clock not set");
        return false;
      }
      resolved = true;
    } else {
      LOG_DBG("CLK", "DNS attempt %d for %s failed", attempt, kNtpHost);
      delay(200);
    }
  }
  if (!resolved) {
    snprintf(server, sizeof(server), "%s", kNtpFallbackIp);
    LOG_ERR("CLK", "DNS failed for %s; using literal server %s", kNtpHost, server);
  }

  configTzTime("UTC0", server, kNtpAltIp1, kNtpAltIp2);
  // Apply the first reply immediately instead of slewing towards it (the default, which leaves the
  // status short of COMPLETED for minutes on a board that has been off).
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  _syncPending = true;
  _syncStartedMs = millis();

  // Bounded wait. This used to spin 50 x 100 ms right here, which held the UI task for the full
  // 5 s (`[LOOP] New max loop duration: 5025 ms` in the device log) and froze the reader after
  // Wi-Fi association. A reachable server answers well inside this window; anything slower is
  // applied by serviceNtpSync() instead of being thrown away.
  int lastStatus = -1;
  for (unsigned long waited = 0; waited < SYNC_INLINE_MS; waited += 100) {
    const int status = static_cast<int>(sntp_get_sync_status());
    if (status != lastStatus) {
      lastStatus = status;
      LOG_INF("CLK", "sntp status -> %d (0=reset 1=completed 2=in progress)", status);
    }
    if (writeRtcIfSynced()) {
      _syncPending = false;
      setTimezone(_savedTz);
      return true;
    }
    delay(100);
  }

  // Display timezone goes back immediately -- the SNTP exchange itself is UTC-only, so restoring it
  // does not affect a reply that lands later.
  setTimezone(_savedTz);
  LOG_INF("CLK", "NTP reply still in flight; will apply when it lands");
  // Try HTTP HERE, before reporting failure. On the operator's network the 204 endpoint answers in
  // 150-190 ms while SNTP exceeds the whole 1.2 s window (and sometimes lands ~9 s later), so
  // returning false and doing HTTP on the next loop tick made the caller report "Sync failed" for a
  // clock that was set 1.4 s afterwards. One bounded HTTP pass, then the caller gets the truth.
  if (syncFromHttpDate()) {
    _syncPending = false;
    _httpFallbackDue = false;
    LOG_INF("CLK", "clock set over HTTP inside the sync call (%lums)", millis() - _syncStartedMs);
    return true;
  }
  // HTTP also failed: leave the flag so serviceNtpSync() retries the fallback and keeps watching for
  // a late UDP reply, which is still the better time source when it does arrive.
  _httpFallbackDue = true;
  return false;
}

void HalClock::serviceNtpSync() {
  if (!_syncPending) return;

  static int lastLateStatus = -1;
  const int status = static_cast<int>(sntp_get_sync_status());
  if (status != lastLateStatus) {
    lastLateStatus = status;
    LOG_INF("CLK", "sntp status -> %d while pending", status);
  }

  if (writeRtcIfSynced()) {
    _syncPending = false;
    setTimezone(_savedTz);
    LOG_INF("CLK", "late NTP reply applied after %lums", millis() - _syncStartedMs);
    return;
  }

  // The inline window expired without a reply: try the HTTP Date fallback once here, off the UI
  // task's bounded call. A late SNTP reply still wins later -- it would just re-write the same
  // clock -- and the flag makes this a single attempt, not a loop.
  if (_httpFallbackDue) {
    _httpFallbackDue = false;
    if (syncFromHttpDate()) {
      _syncPending = false;
      setTimezone(_savedTz);
      LOG_INF("CLK", "clock set over HTTP after the NTP window (%lums)", millis() - _syncStartedMs);
      return;
    }
  }

  if (millis() - _syncStartedMs > SYNC_ABANDON_MS) {
    _syncPending = false;
    setTimezone(_savedTz);
    LOG_ERR("CLK", "NTP sync timed out");
  }
}

