#pragma once

#include <Logging.h>

// Tap rect of the back button BaseTheme::drawHeader paints on touch boards.
// The header's FreeInkUI frame is non-interactive, so the rect is recorded
// here at draw time and consumed by MappedInputManager's Back mapping; every
// screen that draws a titled header gets tap-to-go-back without its own
// routing. Cleared on activity exit (and by headerless draws) so a stale rect
// never eats taps on a new screen. Written by the render task and read by the
// loop task; a torn int read at worst misroutes one tap on a band that is
// being redrawn, so no lock is taken.
namespace HeaderBackTapTarget {
inline int x = 0;
inline int y = 0;
inline int w = 0;
inline int h = 0;

inline void set(const int newX, const int newY, const int newW, const int newH) {
  // Logged only when the rect changes: a tap inside this band is dropped as a tap and mapped to
  // Back, so a stale or oversized rect silently eats taps. On Home a Back press opens the most
  // recently read book, which is exactly the "tapping File transfer opened my book" symptom.
  if (newX != x || newY != y || newW != w || newH != h) {
    LOG_DBG("UITGT", "back tap rect %d,%d %dx%d", newX, newY, newW, newH);
  }
  x = newX;
  y = newY;
  w = newW;
  h = newH;
}

inline void clear() {
  if (w != 0) {
    LOG_DBG("UITGT", "back tap rect cleared");
  }
  w = 0;
}

inline bool contains(const int tx, const int ty) { return w > 0 && tx >= x && tx < x + w && ty >= y && ty < y + h; }
}  // namespace HeaderBackTapTarget
