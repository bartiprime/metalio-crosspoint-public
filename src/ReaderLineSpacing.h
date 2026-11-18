#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ReaderLineSpacingMath.h"

// Device-side helpers for the percent-based line height. The numeric contract
// (range, ladder mapping, multiplier) lives in ReaderLineSpacingMath.h so it can
// be host-tested without the settings/UI headers; the setting itself is
// CrossPointSettings::lineSpacingPercent.
namespace lineSpacing {

// "120%"
inline std::string label(const uint8_t percent) { return std::to_string(clampPercent(percent)) + "%"; }

// A 1%-step picker window around the current value. The option dialog caps out
// at 16 entries, so the popup shows the values within +/-7% of the current one
// (shifted inward at the ends of the range); reopening after a selection
// re-centres the window, which gives 1% steps without a 101-row list.
inline constexpr int kWindowRadius = 7;

struct Window {
  std::vector<std::string> labels;
  uint8_t first = kDefaultPercent;
  int current = 0;

  uint8_t percentAt(const int index) const { return static_cast<uint8_t>(first + index); }
};

inline Window window(const uint8_t percent) {
  constexpr int kMin = kMinPercent;
  constexpr int kMax = kMaxPercent;
  const int cur = clampPercent(percent);
  int first = cur - kWindowRadius;
  int last = cur + kWindowRadius;
  if (first < kMin) {
    last += kMin - first;
    first = kMin;
  }
  if (last > kMax) {
    first -= last - kMax;
    last = kMax;
  }
  first = std::max(first, kMin);
  Window w;
  w.first = static_cast<uint8_t>(first);
  w.labels.reserve(static_cast<size_t>(last - first + 1));
  for (int p = first; p <= last; ++p) w.labels.push_back(label(static_cast<uint8_t>(p)));
  w.current = cur - first;
  return w;
}

}  // namespace lineSpacing
