#pragma once

#include <cstddef>
#include <cstdint>

// r163 3.3 -- the arithmetic behind the reading-statistics page.
//
// WHY IT IS SEPARATE FROM THE STORE: these are the rules the page's numbers must obey
// (what a page-per-hour rate means, when we refuse to print one, how a counter saturates,
// how a duration is written for a one-line row). The store and the screen both include
// this header and nothing here touches Arduino, JSON or the SD card, so the rules are
// host-tested (test/reading_stats) instead of argued about on the glass.
//
// WHAT IS COUNTED, AND WHY THE RATE IS CONSERVATIVE. Pages are successful FORWARD page
// turns; time is the dwell between two of them, clamped per dwell and only counted when
// the dwell is at least kMinDwellMs (a skimmed page is a page turned without a read --
// counting its 300 ms would inflate the rate wildly). The last page of a session is
// never counted, because no turn follows it. Every omission makes time SMALLER, so a
// rate that prints is a floor, not a flattering average.
//
// THE BOUND, and it is enforced in three places (store, restore clamp, this header):
//   * kMaxBooks entries in the store, most recently read first. The rest of the payload
//     is a lifetime total that outlives an evicted entry, so evicting a book never
//     rewrites history for the "All books" block.
//   * kMaxPagesPerBook pages and kMaxSecondsPerBook seconds per book, saturated rather
//     than wrapped. At ~1.5 pages/s an honest reader needs thousands of hours to reach
//     the page cap, and the seconds cap is ~3.2 years of continuous reading.
namespace readingstats {

// Books kept per card. A dozen covers a heavy reading season; past that the oldest entry
// is dropped and only its contribution to the lifetime total survives.
inline constexpr int kMaxBooks = 12;

// Per-entry saturation points. Both are also the restore-clamp bounds in
// lib/Backup/BackupProfile.h -- change one, change the other.
inline constexpr uint32_t kMaxPagesPerBook = 10'000'000u;
inline constexpr uint32_t kMaxSecondsPerBook = 100'000'000u;

// A dwell shorter than this is a skim, not a read: the page counts, the time does not.
inline constexpr uint32_t kMinDwellMs = 2000;

// One dwell is capped at this: a page left open on the table (or a screen woken in a
// bag) must not buy an hour of "reading". Matches ReaderSession's own cap.
inline constexpr uint32_t kMaxDwellMs = 30u * 60u * 1000u;

// Below this much recorded time a pages-per-hour figure is arithmetic on noise (one page
// in three seconds reads as 1200/h). The page prints "--" instead.
inline constexpr uint32_t kMinRateSeconds = 60;

struct Tally {
  uint32_t pages = 0;
  uint32_t seconds = 0;
};

// base + add, saturated at cap (never wraps).
uint32_t saturatingAdd(uint32_t base, uint32_t add, uint32_t cap);

// Accumulate one session's contribution into a tally, saturating both counters.
void accumulate(Tally& tally, uint32_t pages, uint32_t seconds);

// Sum `add` into `into`, saturating both counters.
void accumulate(Tally& into, const Tally& add);

// True when there is enough recorded time for a rate to mean anything.
bool haveRate(const Tally& tally);

// pages / hours. Only meaningful when haveRate(); callers must ask first.
float pagesPerHour(const Tally& tally);

// Writes the rate as "56.3" (one decimal under 100/h, whole numbers above, so a row
// never gains a character just because the reader was fast), or "--" when haveRate() is
// false. Never writes past `size` and always NUL-terminates.
void formatPagesPerHour(const Tally& tally, char* buf, size_t size);

// "0m" / "42m" / "3h 07m" / "1032h" -- minutes under an hour, hours+minutes above, whole
// hours once the minutes stop fitting a row.
void formatDuration(uint32_t seconds, char* buf, size_t size);

}  // namespace readingstats
