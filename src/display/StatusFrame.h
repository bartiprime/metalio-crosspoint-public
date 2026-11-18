#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <fontIds.h>

// StatusFrame — the ONE place a "the device is working" frame is composed.
//
// WHY THIS FILE EXISTS. Every blank/white intermediate frame in this tree has been
// classified (site table in docs/engineering/R99_8_BLANK_FRAMES.md) as one of two
// things, and only the first kind may use this helper:
//
//   (a) a DELIBERATE clear that is part of a clean-paint sequence -- the frame is a
//       drive, and its content is the only thing that can be changed; or
//   (b) a GAP where nothing was driven at all and the panel simply held the previous
//       frame -- filling that with a message means ADDING a drive, which is the
//       regression the operator ruled out, so it is out of scope here.
//
// CONTRACT — all three must hold at the call site, or the frame is a regression:
//
//   1. A drive is ALREADY committed at this point (this helper must not add one).
//      It writes the message into the frame that drive was about to carry, so the
//      drive count, the waveform and the refresh latch are exactly what they were.
//      If a call site has no drive of its own, it must NOT use this helper.
//
//   2. The arriving screen's next paint is a full/clean one, so the text can never
//      survive as the "previous" frame a later differential drive targets. In
//      practice: only on a path that lands on Home or on a reader entry, both of
//      which arm forceCleanNextRefresh() (ActivityManager::goHome's arrival arm and
//      ReaderActivity::onEnter's arm). This is the r99.7 guarantee, unchanged.
//
//   3. No threshold is needed, because the drive is unconditional: there is no fast
//      path on which the message could flash for 100 ms. Do not call this from a
//      snappy transition to "just in case".
namespace statusframe {

// Compose `message` as a whole-panel status frame and commit it with `mode`, which
// must be the mode the call site was already going to pass to displayBuffer().
// Reuses the plain text path (drawCenteredText) -- the same ~165 ms one-line compose
// as any other text frame, no popup chrome, no new renderer, no new state.
inline void paint(GfxRenderer& renderer, const char* message, const HalDisplay::RefreshMode mode) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, message);
  renderer.displayBuffer(mode);
}

}  // namespace statusframe
