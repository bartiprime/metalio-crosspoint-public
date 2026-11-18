#pragma once

// ---------------------------------------------------------------------------
// Screen geometry for the web API.
//
// The panel has two sizes and they are not interchangeable:
//
//   native   the drive geometry, as the EPD controller sees it. On the Metalio
//            E-Ink4-Plus this is 1216x684 (metalio_driver/display/displays.c,
//            ED047TC2_1216) -- LANDSCAPE.
//   portrait the same panel used upright: 684x1216. That is the orientation a
//            page of a book is laid out in, and it is the one every consumer of
//            this value wants (see below).
//
// What consumes it: the browser-side EPUB image optimiser in FilesPage.html
// sizes inline images against `MAX_WIDTH x MAX_HEIGHT`, a PORTRAIT short-edge x
// long-edge pair. Its device table knew only 480x800 (X4) and 528x792 (X3), so a
// device it did not recognise -- ours -- silently fell back to 800x480 and
// scaled every in-book image for a panel 33 % smaller in both axes. /api/status
// now reports the real numbers so that fallback is not taken.
//
// Deliberately a pure function of the two ints the HAL reports at runtime, not a
// pair of literals: a panel change (or a different board in the fork) cannot
// desync the API from the glass.
// ---------------------------------------------------------------------------

namespace cp_web {

struct ScreenSize {
  int width;   // short edge -- portrait width
  int height;  // long edge -- portrait height
};

// Native (landscape, as the controller sees it) -> portrait (short x long). The
// result is orientation-independent: it does not care which way up the UI is
// currently rotated, only what a page is laid out on.
constexpr ScreenSize screenPortrait(const int nativeWidth, const int nativeHeight) {
  if (nativeWidth <= 0 || nativeHeight <= 0) return ScreenSize{0, 0};
  return (nativeWidth <= nativeHeight) ? ScreenSize{nativeWidth, nativeHeight}
                                       : ScreenSize{nativeHeight, nativeWidth};
}

}  // namespace cp_web
