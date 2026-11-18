#include "ReadingStats.h"

#include <algorithm>
#include <cstdio>

namespace readingstats {

uint32_t saturatingAdd(const uint32_t base, const uint32_t add, const uint32_t cap) {
  if (add >= cap || base >= cap - add) return cap;
  return base + add;
}

void accumulate(Tally& tally, const uint32_t pages, const uint32_t seconds) {
  tally.pages = saturatingAdd(tally.pages, pages, kMaxPagesPerBook);
  tally.seconds = saturatingAdd(tally.seconds, seconds, kMaxSecondsPerBook);
}

void accumulate(Tally& into, const Tally& add) { accumulate(into, add.pages, add.seconds); }

bool haveRate(const Tally& tally) { return tally.seconds >= kMinRateSeconds && tally.pages > 0; }

float pagesPerHour(const Tally& tally) {
  if (tally.seconds == 0) return 0.0f;
  return (static_cast<float>(tally.pages) * 3600.0f) / static_cast<float>(tally.seconds);
}

void formatPagesPerHour(const Tally& tally, char* buf, const size_t size) {
  if (buf == nullptr || size == 0) return;
  if (!haveRate(tally)) {
    std::snprintf(buf, size, "--");
    return;
  }
  const float rate = pagesPerHour(tally);
  if (rate >= 100.0f) {
    // No decimal above 100/h: the row is a fixed width and the fraction is noise there.
    std::snprintf(buf, size, "%.0f", static_cast<double>(rate));
  } else {
    std::snprintf(buf, size, "%.1f", static_cast<double>(rate));
  }
}

void formatDuration(const uint32_t seconds, char* buf, const size_t size) {
  if (buf == nullptr || size == 0) return;
  const uint32_t totalMinutes = seconds / 60u;
  const uint32_t hours = totalMinutes / 60u;
  const uint32_t minutes = totalMinutes % 60u;
  if (hours == 0) {
    std::snprintf(buf, size, "%lum", static_cast<unsigned long>(minutes));
  } else if (hours >= 1000u) {
    // Past four digits the row never fits "999h 59m"; whole hours stay inside it.
    std::snprintf(buf, size, "%luh", static_cast<unsigned long>(hours));
  } else {
    std::snprintf(buf, size, "%luh %02lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

}  // namespace readingstats
