#include "LibraryIndexActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// LIB5 (operator, 6 Nov 2026: "every single book index causes a screen refresh,
// we might want to change this so it doesnt flash as much"). The cadence is a
// budget of STEPPING between drives, measured from the moment the last drive
// RETURNED -- see lastDriveEndMs for why that distinction is the whole fix. A
// HALF drive on this panel is ~899 ms, so 600 ms of stepping below it is ~10
// passes of up to 6 books each: one flash per ~60 books instead of one per book.
//
// LIB5b keeps this gate and adds the second term: the key the panel would print has to
// have MOVED as well (see progressKey()). Cadence is the intersection of the two, never a
// queue, so a slow drive can only make the panel update later, never faster.
constexpr uint32_t PROGRESS_REPAINT_MS = 600;

// LIB5b: what the ETA is allowed to be based on. A rate computed from two books is a guess
// wearing a number's clothes; below these floors the line prints NOTHING, which is worse to
// look at and honest. 5 books and 1 s, as specified.
constexpr int kEtaMinBooks = 5;
constexpr uint32_t kEtaMinMs = 1000;

// The count line sits this far under the popup's own caption. It is the same offset the frame
// has always used (drawPopup's box + 22), now named because the tick's erase band is derived
// from it.
constexpr int kCountLineOffsetY = 22;

// LIB5b: the vertical layout of the ONLY part of the progress frame that changes -- the count
// line, the bar under it, and the ETA under the bar. Everything else on the screen (header,
// popup frame, caption, Cancel) is static and is composed once per walk.
//
// Deriving every edge from `box` and the theme's metrics is what keeps the tick's erase band
// from ever covering the frame it sits under: `top` is clamped to below the frame's own
// bottom edge, so a tick cannot white out the popup border even if a theme changes the gap.
struct ProgressBand {
  int countY = 0;
  int barY = 0;
  int barH = 0;
  int etaY = 0;
  int top = 0;
  int bottom = 0;
};

ProgressBand progressBand(const GfxRenderer& renderer, const Rect& box) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int gap = metrics.topPadding;
  ProgressBand band;
  band.countY = box.y + box.height + kCountLineOffsetY;
  const int countH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  band.barH = metrics.popupProgressBarHeight > 2 ? metrics.popupProgressBarHeight : 2;
  band.barY = band.countY + countH + gap;
  band.etaY = band.barY + band.barH + gap;
  const int etaH = renderer.getLineHeight(NOTOSANS_14_FONT_ID);
  // The frame is drawn OUTSIDE the box (drawPopup puts frameThickness of black above and below
  // it), so the band may not start before the frame's last row.
  const int boxFrameBottom = box.y + box.height + metrics.popupFrameThickness * 2;
  const int wantTop = band.countY - gap;
  band.top = wantTop > boxFrameBottom ? wantTop : boxFrameBottom;
  band.bottom = band.etaY + etaH + gap;
  return band;
}

// The bar fills by the SAME integer percentage the count prints, so the two cannot disagree.
// Solid outline + fill from the theme's own progress tokens: it is the theme's bar, moved from
// inside the popup to under the count so the ETA has somewhere to sit under it.
void drawProgressBar(const GfxRenderer& renderer, const Rect& box, const ProgressBand& band, const int percent) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barWidth = box.width - metrics.popupMarginX * 2;
  if (barWidth <= 0 || band.barH <= 0) return;
  const int barX = box.x + (box.width - barWidth) / 2;
  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, band.barY, barWidth, band.barH, 1, metrics.popupProgressOutlineInverted);
  }
  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int fillWidth = barWidth * clamped / 100;
  if (fillWidth > 0) renderer.fillRect(barX, band.barY, fillWidth, band.barH, metrics.popupProgressFillInverted);
}

// The Cancel control: one full-width row, theme row height (86 px on this panel, far past the
// 44 px SDK touch floor). Drawn by the walk's entry frame and NOT by a tick -- see
// requirement 3: the tick's band ends well above it, and paintProgress() only repaints this
// control when a tick's band would overlap it (which today's geometry says it never does).
void drawCancelControl(const GfxRenderer& renderer, const Rect& cancel) {
  renderer.drawRect(cancel.x, cancel.y, cancel.width, cancel.height, 2, true);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID,
                            cancel.y + (cancel.height - renderer.getLineHeight(NOTOSANS_18_FONT_ID)) / 2,
                            tr(STR_CANCEL), true, EpdFontFamily::REGULAR);
}

bool rectsOverlap(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

Rect rectUnion(const Rect& a, const Rect& b) {
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.width) > (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
  const int y1 = (a.y + a.height) > (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect clampRect(const Rect& r, const int screenW, const int screenH) {
  const int x0 = r.x < 0 ? 0 : r.x;
  const int y0 = r.y < 0 ? 0 : r.y;
  const int x1 = (r.x + r.width) > screenW ? screenW : (r.x + r.width);
  const int y1 = (r.y + r.height) > screenH ? screenH : (r.y + r.height);
  if (x1 <= x0 || y1 <= y0) return Rect{x0, y0, 0, 0};
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

LibraryIndexActivity::LibraryIndexActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const bool startImmediately)
    : Activity("LibraryIndex", renderer, mappedInput), autoStart(startImmediately) {
  // D2: this screen owns its refresh cadence. Activity::requestUpdate() otherwise
  // hands every paint to the in-screen transition setting (default AUTO), and an
  // AUTO tick measures a few changed BYTES -- the count digits and the bar inside
  // one band -- demotes the drive to FAST, and FAST over a frame carrying
  // anti-aliased grey selects the vendor's erase-then-grey table, which drives
  // every pixel of the panel to paper before the grey ramp starts. That is the
  // whole-panel wipe the operator saw once per tick (metalio_vendor_panel.cpp:
  // useGray8 -> my_waveform_wipe_gl(3) -> epd_hl_update_screen_full(), and
  // highlevel.c sets dirty_lines 684/684 there). The readers opt out for the same
  // reason: a global setting must not rewrite a cadence a screen tuned on glass.
  transitionRefreshPolicy_ = false;
}

LibraryIndexActivity::~LibraryIndexActivity() = default;

void LibraryIndexActivity::onEnter() {
  Activity::onEnter();
  if (autoStart) {
    start(Mode::Update);
  } else {
    showStatus();
  }
  requestUpdate();
}

void LibraryIndexActivity::onExit() {
  // Leaving mid-build is not offered by the UI, but a teardown here is what
  // keeps a popped activity from holding the journal open behind the shelf.
  stepper.reset();
  Activity::onExit();
}

std::string LibraryIndexActivity::statusText() {
  library::IndexSummary summary;
  library::PartialSummary part;
  const bool haveIndex = library::readLibraryIndexSummary(summary);
  const bool havePartial = library::readLibraryPartialSummary(part);

  char line[96];
  std::string out;
  if (haveIndex) {
    std::snprintf(line, sizeof(line), "%u books, %u folders", static_cast<unsigned>(summary.books),
                  static_cast<unsigned>(summary.folders));
    out = line;
    const unsigned generation[] = {static_cast<unsigned>(summary.generation)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_GENERATION), generation, 1);
    out += "\n";
    out += line;
  } else {
    out = tr(STR_LIBRARY_INDEX_NONE);
  }
  if (havePartial) {
    out += "\n";
    if (part.total > 0) {
      const unsigned counts[] = {static_cast<unsigned>(part.records), static_cast<unsigned>(part.total)};
      i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_PAUSED_FMT), counts, 2);
    } else {
      const unsigned counts[] = {static_cast<unsigned>(part.records)};
      i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_PAUSED_UNKNOWN_FMT), counts, 1);
    }
    out += line;
  }
  return out;
}

void LibraryIndexActivity::showStatus() {
  state = State::STATUS;
  statusBuffer = statusText();
  const char* options[] = {tr(STR_UPDATE), tr(STR_LIBRARY_INDEX_REBUILD), tr(STR_LIBRARY_INDEX_REPAIR), tr(STR_CANCEL)};
  popup.showMessage(tr(STR_LIBRARY_INDEX), statusBuffer.c_str(), options, 4, 0, [this](const int index) {
    switch (index) {
      case 0:
        start(Mode::Update);
        break;
      case 1:
        start(Mode::Rebuild);
        break;
      case 2:
        start(Mode::Repair);
        break;
      default:
        finish();
        break;
    }
  });
}

void LibraryIndexActivity::start(const Mode wanted) {
  mode = wanted;
  // A-P2-7: the journal is deliberately NOT discarded here any more. A Rebuild
  // used to throw it away before begin() had proved it could start, so the
  // cheapest failure available -- card full, card gone -- destroyed minutes of
  // books already walked and journaled. begin() now sees the journal and decides
  // for itself: LibraryPartial::open truncates a journal whose identity does not
  // match (generation, base generation, card id, root path, schema, fold
  // version, stride) and resumes one that does, and a resume of this identity is
  // the same walk over the same card. Repair is still a from-scratch walk
  // because removing the index below resets the base generation to 0, which the
  // identity check then fails on.
  if (mode == Mode::Repair) {
    // The index itself is suspect, so arrival history from it cannot be trusted
    // either. This is the only mode that starts with no previous index at all.
    Storage.remove(library::libraryIndexPath());
  }

  stepper = makeUniqueNoThrow<library::LibraryIndexStepper>();
  // Only Update asks before promoting: it is the incremental path, and the point
  // of the diff is the reader's own choice. Rebuild and Repair were chosen from a
  // menu that already said what they do, so they force.
  const bool confirm = mode == Mode::Update;
  if (!stepper || !stepper->begin("/", SETTINGS.libraryUseMetadata != 0, confirm)) {
    LOG_ERR("LIBIDX", "cannot start the index build");
    state = State::FAILED;
    requestUpdate();
    return;
  }
  state = State::BUILDING;
  lastDriveEndMs.store(0);
  // D3: start the denominator. It is a folder walk that opens no book, stepped from
  // drive() at the same bounded cadence as the walk, and it publishes the count as
  // the walk's total before the first parse -- which is what gives the bar, the ETA
  // and the percent cadence a denominator for the whole run instead of only at its
  // end. Skipped when the journal already knows its total (a completed resume): the
  // number is on the card and a second folder walk would only re-prove it.
  if (stepper->progress().total == 0) {
    preCounter = makeUniqueNoThrow<library::LibraryPreCounter>();
    if (preCounter) {
      preCounter->begin("/");  // the same root the stepper is given, one line above
    } else {
      LOG_ERR("LIBIDX", "OOM: pre-count; the walk runs with no denominator");
    }
  }
  // LIB5b: a new walk means a new static frame (chrome is composed by the first paint) and a
  // new rate clock. The key is seeded to what that first paint will show, so the entry frame
  // is not immediately followed by a tick that would print the same numbers.
  progressChromeValid.store(false, std::memory_order_relaxed);
  resetWalkClock();
  lastPaintedKey = progressKey();
  requestUpdate();
}

void LibraryIndexActivity::drive() {
  if (!stepper) return;

  // Cancel is a request, not a teardown: the stepper finishes the book it is
  // holding and stops at the next boundary, so the journal stays consistent.
  bool cancelThisPass = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    stepper->requestCancel();
    cancelThisPass = true;
  }
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    const Rect cancel = cancelRect();
    if (tapX >= cancel.x && tapX < cancel.x + cancel.width && tapY >= cancel.y && tapY < cancel.y + cancel.height) {
      stepper->requestCancel();
      cancelThisPass = true;
    }
  }

  // D3: the denominator, before any book is parsed. Same cadence and the same
  // Cancel test as the walk below -- a bounded number of directory entries per
  // pass, never a whole-card scan in one go -- so the screen stays responsive and
  // Back works during the count too. Nothing is parsed on a pass that is still
  // counting, and the walk below is not touched by this: the count decides nothing
  // about what gets indexed.
  if (preCounter) {
    if (cancelThisPass) {
      // The reader stopped the build; a number nobody will see is not worth the
      // card reads. Dropping the counter lets the walk's own cancel path run.
      preCounter.reset();
      LOG_INF("LIBIDX", "pre-count abandoned: cancel requested");
    } else {
      const uint32_t countStart = millis();
      for (int i = 0; i < 6 && !preCounter->finished(); i++) {
        preCounter->step();
        if (millis() - countStart > 60) break;
      }
      if (!preCounter->finished()) return;  // more to count: nothing parsed this pass
      if (preCounter->failed()) {
        // The card could not be enumerated. That is NOT a build failure: drop the
        // denominator and run exactly the walk r101.18 ran (empty bar, per-book
        // cadence, ETA absent) rather than hold the build up for a number.
        LOG_ERR("LIBIDX", "pre-count failed; the walk runs with no denominator");
      } else {
        stepper->publishTotal(preCounter->count());
      }
      // One shot either way: from here the walk owns the pass, and a late file
      // add/remove is corrected by finishScan()'s own total.
      preCounter.reset();
    }
  }

  // Six books or 60 ms per pass, whichever comes first: enough to make progress
  // between paints, short enough that the Back key is never more than one book
  // away from being noticed.
  const uint32_t passStart = millis();
  library::BuildStep step = library::BuildStep::Progress;
  for (int i = 0; i < 6; i++) {
    step = stepper->step();
    if (step != library::BuildStep::Progress) break;
    if (millis() - passStart > 60) break;
  }

  switch (step) {
    case library::BuildStep::ReadyToCommit:
      state = State::CONFIRM;
      requestUpdate();
      return;
    case library::BuildStep::Done:
      state = State::DONE;
      requestUpdate();
      return;
    case library::BuildStep::Cancelled:
      state = State::CANCELLED;
      requestUpdate();
      return;
    case library::BuildStep::Failed:
      state = State::FAILED;
      requestUpdate();
      return;
    default:
      break;
  }

  // LIB5: coalesce, do not drop. A frame is asked for only when BOTH hold:
  //   * at least PROGRESS_REPAINT_MS of stepping since the last drive RETURNED,
  //     so the drive's own ~899 ms can never pay for the next one, and
  //   * the key the bar and the count would print has actually moved (progressKey()).
  //
  // LIB5b: the key also encodes the CADENCE, by library size:
  //   < 100 books   -> every book: the count IS the useful granularity, and there are few frames
  //   100 .. 200    -> every 1 % of the total
  //   > 200         -> every 5 % of the total
  // The threshold is on the TOTAL the walk knows, and the steps are of that total, not of the
  // books walked. A walk with no denominator (total == 0 while the scan runs -- see
  // BuildProgress) can only key on the book count, which is the number it prints.
  //
  // State changes never come through here: start, the confirm pause, its
  // resume, done, cancelled and failed each request their own frame directly,
  // so the FINAL frame of every run is always driven.
  const int key = progressKey();
  if (key != lastPaintedKey && millis() - lastDriveEndMs.load() >= PROGRESS_REPAINT_MS) {
    lastPaintedKey = key;
    requestUpdate();
  }
}

// LIB5b: what the panel would print, as one integer. The same function feeds the cadence gate
// and the seed taken when a walk (re)starts, so "already painted" and "worth painting" can
// never be computed from different rules.
int LibraryIndexActivity::progressKey() const {
  if (!stepper) return -1;
  const auto progress = stepper->progress();
  const int books = static_cast<int>(progress.books);
  if (progress.total == 0) return books;              // no denominator yet: the count is the key
  if (progress.total < 100) return books;             // per book
  const int percent = books * 100 / static_cast<int>(progress.total);
  if (progress.total <= 200) return percent;          // per 1 %
  return (percent / 5) * 5;                           // per 5 %
}

// LIB5b: the rate's baseline, taken whenever the walk actually walks. books is cumulative
// across a resume, so the count at this instant is subtracted from it: the rate must be
// "books this screen watched go by" over "time this screen spent walking", not an average
// that includes the journal's prefix or the reader's time in the confirm dialog.
void LibraryIndexActivity::resetWalkClock() {
  walkStartMs = millis();
  walkStartBooks = stepper ? stepper->progress().books : 0;
}

void LibraryIndexActivity::loop() {
  if (state == State::STATUS) {
    popup.handleInput(mappedInput, [this] { requestUpdate(); });
    // The selection callback can move us straight into BUILDING.
    if (!popup.isActive() && state == State::STATUS) finish();
    return;
  }
  if (state == State::BUILDING) {
    drive();
    return;
  }
  if (state == State::CONFIRM) {
    // Nothing steps here. Confirm saves the walk, Back throws it away; either way
    // the journal already holds every book, so a wrong choice costs a second, not
    // a rebuild.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      stepper->commit();
      state = State::BUILDING;
      // LIB5b: the diff frame replaced the walk's frame, so the chrome has to be composed
      // again, and the rate clock restarts HERE -- the reader's time in this dialog is not
      // book-walking time and must not stretch the ETA.
      progressChromeValid.store(false, std::memory_order_relaxed);
      resetWalkClock();
      lastPaintedKey = progressKey();
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      stepper->requestCancel();
      state = State::BUILDING;
      progressChromeValid.store(false, std::memory_order_relaxed);
      resetWalkClock();
      lastPaintedKey = progressKey();
      requestUpdate();
      return;
    }
    int tapX = 0;
    int tapY = 0;
    if (mappedInput.wasScreenTapped(tapX, tapY)) {
      const auto inside = [tapX, tapY](const Rect& r) {
        return tapX >= r.x && tapX < r.x + r.width && tapY >= r.y && tapY < r.y + r.height;
      };
      if (inside(applyRect())) {
        stepper->commit();
        state = State::BUILDING;
        requestUpdate();
      } else if (inside(cancelRect())) {
        stepper->requestCancel();
        state = State::BUILDING;
        requestUpdate();
      }
    }
    return;
  }
  // DONE / CANCELLED / FAILED: any dismissal goes back, and the screen never
  // pops itself, because the reader may want to read the counts.
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) finish();
}

Rect LibraryIndexActivity::cancelRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.popupMarginX * 2;
  // The theme's own menu row height, not a literal: on this panel it is 86 px,
  // which clears the operator's 66 px floor (1.5x the 44 px SDK touch minimum)
  // with room to spare, and a theme that states a shorter row states its intent.
  const int height = metrics.menuRowHeight;
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - height - metrics.topPadding;
  return Rect{metrics.popupMarginX, y, width, height};
}

Rect LibraryIndexActivity::applyRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect cancel = cancelRect();
  // Directly above Cancel, separated by the theme's own row gap. Both rows are
  // menuRowHeight (86 px on this panel), so both clear the 66 px floor.
  const int gap = metrics.topPadding;
  return Rect{cancel.x, cancel.y - cancel.height - gap, cancel.width, cancel.height};
}

// Draw a line that is guaranteed to fit the panel. The counts line is the only
// string on this screen whose width depends on the library ("+1,204 new" is
// wider than "+3 new"), and a clipped number reads as a wrong number.
//
// The truncation is the renderer's own (UTF-8 safe, ellipsis included), the same
// one BaseTheme::drawPopup uses for its caption — one implementation, so the
// counts line and the caption cannot disagree about where the panel ends.
void LibraryIndexActivity::drawFittedLine(const int fontId, const int y, const char* text, const bool centered) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int maxWidth = renderer.getScreenWidth() - metrics.popupMarginX * 2;
  const std::string line = renderer.truncatedText(fontId, text != nullptr ? text : "", maxWidth);
  if (centered) {
    renderer.drawCenteredText(fontId, y, line.c_str(), true, EpdFontFamily::REGULAR);
  } else {
    renderer.drawText(fontId, metrics.popupMarginX, y, line.c_str(), true, EpdFontFamily::REGULAR);
  }
}

// The frame before the frame that matters: what this update would change. It is
// drawn from the stepper's own counts and nothing else, so it cannot disagree
// with what the commit then does.
void LibraryIndexActivity::paintDiff() {
  const auto progress = stepper->progress();
  const unsigned diffCounts[] = {static_cast<unsigned>(progress.added), static_cast<unsigned>(progress.changed),
                                 static_cast<unsigned>(progress.removed), static_cast<unsigned>(progress.unchanged)};
  i18nFormatUnsigned(countsBuffer, sizeof(countsBuffer), tr(STR_LIBRARY_INDEX_DIFF_FMT), diffCounts, 4);
  countsLine = countsBuffer;

  const Rect box = GUI.drawPopup(renderer, tr(STR_LIBRARY_INDEX_COMPARE), /*commit=*/false);
  drawFittedLine(NOTOSANS_18_FONT_ID, box.y + box.height + 22, countsLine.c_str(), /*centered=*/true);

  const Rect apply = applyRect();
  renderer.drawRect(apply.x, apply.y, apply.width, apply.height, 2, true);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID,
                            apply.y + (apply.height - renderer.getLineHeight(NOTOSANS_18_FONT_ID)) / 2,
                            tr(STR_LIBRARY_INDEX_APPLY), true, EpdFontFamily::REGULAR);

  const Rect cancel = cancelRect();
  renderer.drawRect(cancel.x, cancel.y, cancel.width, cancel.height, 2, true);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID,
                            cancel.y + (cancel.height - renderer.getLineHeight(NOTOSANS_18_FONT_ID)) / 2, tr(STR_CANCEL),
                            true, EpdFontFamily::REGULAR);
  // QA2 (r101.9): a cleared canvas replaced whole wants HALF, not displayBuffer()'s FAST default.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

// LIB5b: the progress frame, in two shapes.
//
// ENTRY (once per walk): the whole frame -- header, popup frame, caption, count, bar, ETA,
// Cancel -- composed onto the canvas render() has just cleared, and driven with one HALF. This
// is the only frame of the walk composed that way.
//
// TICK (every later repaint): ONLY the value band (count line, bar, ETA), composed over the
// framebuffer the entry frame left behind. There is no clearScreen() here, which is the first
// half of the fix: the panel's drive is limited by the diff between this framebuffer and the
// frame the glass holds (highlevel.c's epd_difference_image_cropped -> the `[mask]` row/column
// mask), so touching only the band means the waveform covers only the band's rows and column
// bytes -- the same mechanism the charging icon uses for its (572,21 21x16) rect.
//
// D2: the second half is the MODE. A diffed band is only a band-sized drive while the request
// survives to the driver as HALF -- see the pin at the bottom of this function, and the reason
// this screen opts out of the in-screen transition policy in its constructor. A FAST demotion
// took the erase-then-grey wipe table, whose erase columns pulse every pixel in the panel, and
// that is what made r101.18's tick a whole-panel wipe (log: mode=gray8-wipe-GL16(38) ms=644
// with [mask] dirty_lines=684/684).
//
// The drive is HALF, not FAST: on this panel a FAST paint is followed by the row-9 completion
// drive (GfxRenderer::displayBuffer, CP_RICKY_COMPLETION_DRIVE), i.e. a SECOND drive per tick,
// and that is the double-flash this screen is being fixed for. HALF is "GC16 over the diff",
// one drive, no completion. The charging icon can afford FAST because it changes twice a
// session; this fires twenty times a walk.
void LibraryIndexActivity::paintProgress() {
  if (!stepper) return;
  const auto progress = stepper->progress();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (progress.total > 0 && progress.books < progress.total) {
    const unsigned counts[] = {static_cast<unsigned>(progress.books), static_cast<unsigned>(progress.total)};
    i18nFormatUnsigned(countsBuffer, sizeof(countsBuffer), tr(STR_LIBRARY_INDEX_COUNT_OF_FMT), counts, 2);
  } else if (progress.total > 0) {
    const unsigned counts[] = {static_cast<unsigned>(progress.total)};
    i18nFormatUnsigned(countsBuffer, sizeof(countsBuffer), tr(STR_LIBRARY_INDEX_COUNT_READY_FMT), counts, 1);
  } else {
    const unsigned counts[] = {static_cast<unsigned>(progress.books)};
    i18nFormatUnsigned(countsBuffer, sizeof(countsBuffer), tr(STR_LIBRARY_INDEX_COUNT_FMT), counts, 1);
  }
  countsLine = countsBuffer;

  // A percentage needs a denominator, and D3 supplies one before the first parse:
  // the pre-count publishes the books this walk will visit (Builder: publishTotal),
  // so `total` is 0 only when that count could not be taken -- and then the bar
  // stays empty rather than inventing a fraction, which is exactly r101.18. The bar
  // and the count are fed from this ONE value, so they cannot disagree, and the
  // clamp is what keeps a card that gained books mid-walk from driving the bar past
  // its end.
  const int percent = progress.total > 0 ? std::min(100, progress.books * 100 / progress.total) : 0;

  if (!progressChromeValid.load(std::memory_order_relaxed)) {
    // The canvas and the header were cleared/drawn by render() before this call: the walk's
    // FIRST frame replaces the whole screen (it arrives from the status popup or the diff
    // frame), and it is the only frame of the walk that is composed that way.
    // commit=false: the chrome is painted, the single drive at the bottom of this block is the
    // only one. (drawPopup's own commit would be one drive, and the values another.)
    const Rect box = GUI.drawPopup(renderer, tr(STR_LIBRARY_INDEXING), /*commit=*/false);
    progressBox = box;
    const ProgressBand band = progressBand(renderer, box);
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, band.countY, countsLine.c_str(), true, EpdFontFamily::REGULAR);
    drawProgressBar(renderer, box, band, percent);
    paintEta(band.etaY);
    const Rect cancel = cancelRect();
    drawCancelControl(renderer, cancel);
    // QA2 (r101.9): a cleared canvas replaced whole wants HALF, not displayBuffer()'s FAST default.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    progressChromeValid.store(true, std::memory_order_relaxed);
    // LIB5: the cadence gate's one stamp -- taken HERE, after the panel is free,
    // because this is the moment the next frame may start being earned.
    lastDriveEndMs.store(millis());
    return;
  }

  // TICK. Lay the band out from the rect the entry frame actually drew, so it cannot drift
  // from the frame it sits under.
  const Rect box = progressBox;
  const ProgressBand band = progressBand(renderer, box);
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  // Requirement 3: the Cancel rect must sit OUTSIDE this region. It does by construction -- the
  // band ends at the ETA, the Cancel row is the bottom row of the panel -- but the guard is
  // kept and it is not dead weight: if a theme's geometry ever put them on top of each other,
  // the tick grows to cover the control AND repaints it below, so a tap can never land on a
  // half-erased button. Hit-testing itself never depended on a repaint: drive() and the confirm
  // branch test the tap against cancelRect() (a pure function of the theme), and the button is
  // armed from the moment the entry frame drew it.
  Rect dirty = clampRect(Rect{metrics.popupMarginX, band.top, screenW - metrics.popupMarginX * 2, band.bottom - band.top},
                         screenW, screenH);
  const Rect cancel = cancelRect();
  const bool repaintCancel = rectsOverlap(dirty, cancel);
  if (repaintCancel) dirty = clampRect(rectUnion(dirty, cancel), screenW, screenH);
  if (dirty.width <= 0 || dirty.height <= 0) return;

  // Paper over the old values, then write the new ones. The erase is what makes the count
  // shrink ("100 of 1200" -> "7 of 1200") instead of leaving tails; it is confined to the band.
  renderer.fillRect(dirty.x, dirty.y, dirty.width, dirty.height, /*state=*/false);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, band.countY, countsLine.c_str(), true, EpdFontFamily::REGULAR);
  drawProgressBar(renderer, box, band, percent);
  paintEta(band.etaY);
  if (repaintCancel) drawCancelControl(renderer, cancel);

  LOG_DBG("LIBIDX", "tick rect=(%d,%d %dx%d) books=%u total=%u band_rows=%d/%d", dirty.x, dirty.y, dirty.width,
          dirty.height, static_cast<unsigned>(progress.books), static_cast<unsigned>(progress.total),
          dirty.height, screenH);
  // D2: pin HALF. The clear was already gone at r101.18 (render() only clears when
  // progressChromeValid is false), so the 684/684 mask in the glass log was NOT a
  // re-composed canvas -- it was epd_hl_update_screen_full(), which marks every row
  // dirty by construction, reached because the drive had been demoted to FAST and a
  // FAST frame carrying grey text takes the erase-then-grey wipe table. HALF is
  // GC16 over the diff, i.e. the band, which is the only region this frame touched.
  // An AUTO armed by an earlier requestUpdate() would demote it right back to FAST,
  // so drop that (and only that) here: an explicit mode armed by a subsystem -- a
  // wake or a USB clean -- is left alone, because it was asked for by something
  // that knows more than this tick does.
  const auto parkedPromotion = renderer.peekPromotedRefresh();
  if (parkedPromotion.armed && parkedPromotion.autoMode) renderer.cancelPromotedRefresh();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  lastDriveEndMs.store(millis());
}

// The ETA under the bar. Computed from the measured rate ONLY -- books this screen watched go
// by, over the time it spent walking -- and printed only once that rate means something:
//   * no denominator (total == 0): nothing, the same rule the bar follows;
//   * fewer than kEtaMinBooks books walked, or under kEtaMinMs of walking: nothing, rather
//     than a number extrapolated from two samples;
//   * the walk is at or past its total: nothing, there is nothing left to estimate.
// Anything it does print is labelled an estimate.
bool LibraryIndexActivity::paintEta(const int etaY) {
  etaLine.clear();
  if (!stepper) return false;
  const auto progress = stepper->progress();
  if (progress.total == 0 || progress.total <= progress.books) return false;
  const uint32_t elapsed = millis() - walkStartMs;
  const int walked = static_cast<int>(progress.books) - static_cast<int>(walkStartBooks);
  if (walked < kEtaMinBooks || elapsed < kEtaMinMs) return false;

  const uint64_t remaining = static_cast<uint64_t>(progress.total - progress.books);
  const uint64_t etaMs = remaining * static_cast<uint64_t>(elapsed) / static_cast<uint64_t>(walked);
  char line[64];
  if (etaMs < 60000u) {
    unsigned secs = static_cast<unsigned>((etaMs + 999u) / 1000u);
    if (secs == 0) secs = 1;
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_ETA_SEC_FMT), &secs, 1);
  } else {
    const unsigned mins = static_cast<unsigned>((etaMs + 59999u) / 60000u);
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_ETA_MIN_FMT), &mins, 1);
  }
  etaLine = line;
  renderer.drawCenteredText(NOTOSANS_14_FONT_ID, etaY, etaLine.c_str(), true);
  return true;
}

void LibraryIndexActivity::render(RenderLock&& lock) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (state == State::BUILDING) {
    // LIB5b: a running walk is dispatched BEFORE the shared clear/header below, because only
    // its FIRST frame wants them. That frame replaces the whole screen -- it arrives from the
    // status popup or the diff frame -- so the canvas is cleared and the header drawn here,
    // once per walk. Every later frame is a TICK: no clear, no header, no chrome. That is the
    // whole fix; clearing here would make each tick a whole-panel drive again.
    if (!progressChromeValid.load(std::memory_order_relaxed)) {
      renderer.clearScreen();
      GUI.drawHeader(renderer,
                     Rect{0, metrics.topPadding, renderer.getScreenWidth(),
                          UITheme::getInstance().getTitledHeaderHeight()},
                     tr(STR_LIBRARY_INDEX));
    }
    paintProgress();
    return;
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, renderer.getScreenWidth(), UITheme::getInstance().getTitledHeaderHeight()},
                 tr(STR_LIBRARY_INDEX));

  if (state == State::STATUS) {
    popup.processRender(renderer, mappedInput);
    return;
  }

  if (state == State::CONFIRM) {
    paintDiff();
    return;
  }

  const int pageHeight = renderer.getScreenHeight();
  if (state == State::DONE) {
    // An Update that found nothing to do is not a rebuild, and saying so is the
    // whole point of showing the diff: the reader walks away knowing the shelf is
    // already current rather than wondering what just took ten seconds.
    const auto& done = stepper->stats();
    const bool noOp = done.added == 0 && done.renamed == 0 && done.changed == 0 && done.removed == 0 &&
                      !done.indexReplaced;
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, pageHeight / 2 - 24,
                              noOp ? tr(STR_LIBRARY_INDEX_UP_TO_DATE) : tr(STR_LIBRARY_REBUILD_DONE), true,
                              EpdFontFamily::BOLD);
    char line[64];
    const unsigned doneCount[] = {static_cast<unsigned>(stepper ? stepper->stats().books : 0)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_COUNT_READY_FMT), doneCount, 1);
    renderer.drawCenteredText(NOTOSANS_14_FONT_ID, pageHeight / 2 + 12, line);
  } else if (state == State::CANCELLED) {
    // The point of the message: nothing was lost. The count is what will be
    // resumed, and it is the same number the next Update starts from.
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, pageHeight / 2 - 24, tr(STR_LIBRARY_INDEX_STOPPED), true,
                              EpdFontFamily::BOLD);
    char line[64];
    const unsigned keptCount[] = {static_cast<unsigned>(stepper ? stepper->progress().books : 0)};
    i18nFormatUnsigned(line, sizeof(line), tr(STR_LIBRARY_INDEX_COUNT_FMT), keptCount, 1);
    renderer.drawCenteredText(NOTOSANS_14_FONT_ID, pageHeight / 2 + 12, line);
  } else {
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, pageHeight / 2 - 24, tr(STR_LIBRARY_REBUILD_FAILED), true,
                              EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // QA2 (r101.9): a cleared canvas replaced whole wants HALF, not displayBuffer()'s FAST default.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
