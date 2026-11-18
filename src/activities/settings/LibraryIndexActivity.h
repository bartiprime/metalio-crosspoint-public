#pragma once

#include <LibraryBuilder.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// LIB1: the library index, as a thing the reader can watch and stop.
//
// The screen exists because the index build is the longest thing this firmware
// does that is not a page turn: on a full card it is minutes of SD reads with
// nothing on the glass. Before this it ran as one blocking call behind a static
// "Reading your books..." popup, so the only choices were "wait" and "wait".
//
// Three modes, and the difference between them is what they throw away:
//
//   Update  keep the journal's completed prefix and resume from it
//   Rebuild discard the journal, walk the card from the start, keep the old
//           index so arrival history ("Recently added") survives
//   Repair  discard the journal AND the index: the escape hatch for an index
//           that is wrong in a way a rebuild cannot fix
//
// Cancel keeps everything already committed: the journal is the durable record
// of it, and the live index answers unchanged until the last book is in.
class LibraryIndexActivity final : public Activity {
 public:
  // `startImmediately` is for the library's defer banner, where the reader has
  // already pressed Start and a second confirmation would be noise.
  LibraryIndexActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool startImmediately = false);
  ~LibraryIndexActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Back is "stop this build", not "leave the screen": the manager must not pop
  // the activity out from under a running walk.
  bool ownsBackKey() const override { return true; }

 private:
  enum class State : uint8_t { STATUS, BUILDING, CONFIRM, DONE, CANCELLED, FAILED };
  enum class Mode : uint8_t { Update, Rebuild, Repair };

  void showStatus();
  void start(Mode mode);
  void drive();
  void paintProgress();
  // LIB5b: the coalescing key of the frame a tick would produce -- the book count while the
  // walk has no denominator, the integer percent in the 100-200 band, the 5 % bucket above it.
  // Shared by drive() (which asks) and start()/the confirm resume (which seed it) so the entry
  // frame and the first tick cannot disagree about what has already been painted.
  int progressKey() const;
  // LIB5b: the rate's denominator. Reset every time the walk actually starts walking -- a
  // confirm pause is not book-walking time, and counting it would make the ETA nonsense.
  void resetWalkClock();
  // The ETA under the bar, from measured rate only. Prints nothing (returns false) until the
  // walk has a denominator, has walked at least kEtaMinBooks books and has spent at least
  // kEtaMinMs walking. Draws at `etaY` and keeps the string alive in etaLine.
  bool paintEta(int etaY);
  // The diff, painted before anything is promoted: "+34 new  12 changed
  // 3 missing  1,204 unchanged". The reader decides with the numbers in front of
  // them, not after.
  void paintDiff();
  // The touch target for Cancel while a build runs. One rect, hit-tested from
  // loop() against the reported tap, so it needs no interaction-table rebuild.
  Rect cancelRect() const;
  // The confirm control sits one theme row above Cancel, so both are >= 66 px and
  // a thumb cannot straddle them.
  Rect applyRect() const;
  // Draw a line at `fontId` and shorten it with an ellipsis rather than letting
  // it run off the panel. The counts line is the one string here whose width
  // depends on the library size.
  void drawFittedLine(int fontId, int y, const char* text, bool centered) const;
  std::string statusText();

  std::unique_ptr<library::LibraryIndexStepper> stepper;
  // D3: the denominator, counted before the walk parses anything. Stepped from
  // drive() on the same cadence as the walk, so the screen keeps drawing and Cancel
  // keeps working during the count. Cleared once its answer is published (or it
  // failed), because the walk is all that is left to do.
  std::unique_ptr<library::LibraryPreCounter> preCounter;
  OptionPopup popup;
  State state = State::STATUS;
  Mode mode = Mode::Update;
  bool autoStart = false;
  // LIB5 (operator, 6 Nov): when the last progress DRIVE FINISHED -- written by
  // the render task as displayBuffer() returns, read by the loop task. The
  // cadence gate compares against THIS, and finishing-vs-requesting is the whole
  // fix: a HALF drive blocks for ~899 ms on this panel, so a stamp taken at
  // REQUEST time was already ~899 ms old when the next pass looked at it, the
  // gate was open on every pass, and the panel flashed once per book even though
  // a 500 ms constant was already in the code.
  std::atomic<uint32_t> lastDriveEndMs{0};
  // The coalescing key of the frame that was last asked for: integer percent
  // once the walk knows its denominator, else the book count (see drive()).
  int lastPaintedKey = -1;
  // Kept alive as strings: drawPopup and drawCenteredText take a c_str and the
  // render task may still be mid-frame when the next count lands.
  std::string statusBuffer;
  char countsBuffer[64] = {};
  std::string countsLine;
  // LIB5b: the ETA line, alive for the same reason as countsLine. Empty means "no rate yet",
  // which is the same thing as "print nothing".
  std::string etaLine;
  // LIB5b: the frame carrying the popup, its caption and the Cancel control is composed ONCE
  // per walk -- at BUILDING entry, and again on the resume from the confirm pause, both of
  // which set this false. A tick (true) repaints only the value band and never the chrome.
  // Atomic because the render task reads it and the loop task clears it.
  std::atomic<bool> progressChromeValid{false};
  // The popup rect the entry frame drew. The tick lays the value band out from the SAME rect
  // instead of recomputing the theme's popup geometry, so the band cannot drift from the frame
  // it sits under.
  Rect progressBox{};
  // The rate's clock, in units of "books this walk has walked": books is cumulative across a
  // resume, so the baseline is taken when the walking starts, not when the screen opened.
  uint32_t walkStartMs = 0;
  uint16_t walkStartBooks = 0;
};
