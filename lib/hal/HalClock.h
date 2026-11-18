#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;
  // Late-arriving NTP replies: syncFromNTP() only waits 1.2 s inline, then leaves the exchange in
  // flight and serviceNtpSync() applies it from the main loop.
  char _savedTz[64] = {0};
  bool _syncPending = false;
  unsigned long _syncStartedMs = 0;
  bool writeRtcIfSynced();
  // Writes the RTC from the system clock. Shared by the SNTP path (which gates it on the SNTP
  // status) and the HTTP-Date fallback (which has no SNTP status to consult).
  bool writeRtcFromSystemTime();
  // Last-resort clock source for networks where UDP/123 cannot work at all: reads the RFC 1123
  // Date header off a plain-HTTP 204 endpoint and sets the system clock and the RTC.
  bool syncFromHttpDate();
  // Bounded literal-IP SNTP attempt used on fake-IP networks, where DNS cannot supply a routable
  // address but UDP/123 to a literal IP may still traverse. Writes the RTC and returns true on reply.
  bool probeLiteralNtp(const char* server);
  // Set when the inline SNTP window expired: serviceNtpSync() tries the HTTP fallback once.
  bool _httpFallbackDue = false;
  // PCF8563 VL (voltage-low) flag, bit 7 of the seconds register (0x02). The
  // chip raises it when its backup supply failed, which means the calendar it
  // is serving is not a record of elapsed time -- it is a stale value frozen
  // when the supply went away. Read on every RTC poll (see localTime()) because
  // it is the only thing that distinguishes "the chip says 2026-11-02" from
  // "the chip last knew 2026-11-02". The SDK's Rtc::now() already REFUSES a
  // read with VL set, so without this flag a dead-backup chip is
  // indistinguishable from an absent one.
  mutable bool _vl = false;
  mutable bool _vlKnown = false;
  // Direct Wire read of the VL bit. Returns false when this board has no
  // PCF8563 (the flag is chip-specific) or the transaction fails; `known`
  // reports which of those happened.
  bool readVoltageLow(bool& known) const;
  // Records a freshly read VL value, logging the transitions (first read, and
  // every 0<->1 change) once rather than on every poll.
  void noteVoltageLow(bool vl) const;

 public:
  // Clears the PCF8563 alarm-enable bit before sleep. Nothing programs an alarm today, so this is a
  // guard: the RTC's INT is one of the sources wired-OR onto GPIO2, and a stray alarm would wake the
  // device through the expander.
  void disableAlarm();

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds
  static constexpr unsigned long SYNC_INLINE_MS = 1200;   // bounded wait so the UI task is not held
  static constexpr unsigned long SYNC_ABANDON_MS = 30000; // give up on a reply after this
  static constexpr unsigned long HTTP_TIME_TIMEOUT_MS = 500;  // per-socket-phase cap for the fallback
  // Total time the HTTP fallback may spend across ALL endpoints. It is a budget rather than an
  // attempt count: the old rule only tried the second host when the first died inside 300 ms, so a
  // primary that TIMED OUT (the common failure) killed every fallback before it ran.
  static constexpr unsigned long HTTP_TOTAL_BUDGET_MS = 1200;
  // Bounded literal-IP SNTP attempt on fake-IP networks, before falling back to HTTP. UDP/123 egress
  // is demonstrably alive on such a LAN for these destinations (a host-side probe answered in 60 ms),
  // and a reply here is both faster and more accurate than an HTTP Date round trip.
  static constexpr unsigned long HTTP_LITERAL_PROBE_MS = 300;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // RTC1: the chip's own statement that its calendar is untrusted (PCF8563 VL,
  // bit 7 of register 0x02). True only once the bit has been read and found
  // set; a board without a PCF8563, or a failed read, is false. Callers that
  // seed the system clock from the RTC (main.cpp) must consult this and NOT
  // adopt the calendar when it is true -- the persisted TrustedTime floor is
  // the better claim in that case.
  bool voltageLow() const { return _vl; }
  // False until the VL bit has been read at least once this boot.
  bool voltageLowKnown() const { return _vlKnown; }

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Returns false if RTC is not available.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Waits at most SYNC_INLINE_MS (~1.2 s) inline, so the caller -- the UI task -- is never held for
  // the full SNTP timeout. Returns true if the RTC was already updated when this returned; a reply
  // that lands later is applied by serviceNtpSync(), which the main loop calls every iteration.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Applies an NTP reply that arrived after syncFromNTP() returned. Cheap no-op when nothing is
  // pending, so it can be called unconditionally from the main loop.
  void serviceNtpSync();
};
