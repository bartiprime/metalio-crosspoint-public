#pragma once

#include <cstddef>
#include <cstdint>

// r163 C1 -- cross-chapter preparation: the next chapter's first page, laid out on an idle frame.
//
// WHAT THIS IS NOT: a task. A chapter build holds the Section the render task paints, rasterises
// into the font cache and can borrow the framebuffer (ChapterHtmlSlimParser's image probes), and
// all three of those are forbidden to a worker by the handoff contract's rule 1
// (ReaderHandoff.h:15-18). So this is idle work on the loop task, under the same Try-acquired
// display guard as backgroundBuildTick() (EpubReaderActivity.cpp:489), and every decision it makes
// lives here, in a form the host can test. The layout itself cannot run on a host -- these
// decisions are the part that can be wrong in a way a test can see.
//
// What is prepared is deliberately small: enough for the target page, not the chapter. The trace
// (docs/engineering/R163_CHAPTER_LOAD_TRACE.md §1f) shows renderBook() blocks only until the
// *target* page exists, so a chapter open needs its first page and nothing else. The expensive,
// separable part is what precedes that page: the zip inflate, the parser prefix, and the first
// layout chunk (trace §1d).

namespace chapterprefetch {

// The chapter being prepared for, and the layout it was laid out under. Two artefacts with equal
// targets are interchangeable; a differing signature means every page in it is the wrong shape.
struct Target {
  int spineIndex = -1;
  uint32_t layoutSignature = 0;

  bool valid() const { return spineIndex >= 0; }
  bool operator==(const Target& other) const {
    return spineIndex == other.spineIndex && layoutSignature == other.layoutSignature;
  }
  bool operator!=(const Target& other) const { return !(*this == other); }
};

// The reader's own state, re-read for every decision. `generation` is the C3 session token
// (ReaderHandoff.h SessionGeneration): it is bumped when -- and only when -- the identity of the
// work changes, so a status-bar repaint or a page turn inside the chapter does not invalidate a
// prepared next chapter, and a chapter jump, a font change or a re-open does.
struct Live {
  int spineIndex = -1;
  int spineCount = 0;
  uint32_t layoutSignature = 0;
  uint32_t generation = 0;
  // False until the first paint has resolved the viewport, i.e. before there is a layout to
  // prepare against.
  bool layoutKnown = false;
  bool nextSpineExists = false;
};

// Everything a prepare must hold, all of it at once, on the frame it prepares. Each field is read
// from the reader; a test flips one at a time to prove the gate is load-bearing.
struct Gates {
  // Paint drained. DisplayLockGuard::owns() -- Try-acquired, so it is the proof that no compose
  // is running, not merely a hope.
  bool displayOwned = false;
  bool frameBuffer = false;
  // Nothing painting over the page, and no deferred chrome refresh that owns the framebuffer.
  bool overlayOpen = false;
  bool overlayRefreshPending = false;
  // A build step lent the framebuffer and it came back blank: nothing may lay out onto it.
  bool pageBufferStale = false;
  // The live chapter: present, not building (a build owns the heap, the card and the shared CSS
  // parser), and not about to start one at its partial watermark.
  bool liveSection = false;
  bool liveBuilding = false;
  bool liveNeedsBuild = false;
  // No glyph scan or prewarm in flight -- it competes for the same heap and the same card.
  bool scanning = false;
  // No turn queued behind the lock, and nothing held down: the next loop frames act on input
  // first.
  bool queuedTurn = false;
  bool inputDown = false;
  // The Bluetooth page turner holds ~50 KB while it runs. A build next to it has run out of
  // memory on this device (EpubReaderActivity.cpp:445-450), and a prefetch is not worth stopping
  // the radio for.
  bool radioHoldsHeap = false;
  size_t freeHeap = 0;
  size_t maxAlloc = 0;
};

// ---- budgets -------------------------------------------------------------------------------
//
// Bytes, not adjectives. The artefact is bounded on both sides: what is read out of the zip, and
// what is written as page records.

// Pages laid out by one attempt. One is all the target page needs (renderBook's loop stops at
// `pageCount > target`, trace §1f); four leaves slack for a two-page spread and a status-bar
// reflow while keeping the record budget at four page records, not a chapter's worth.
constexpr int kPagesPerAttempt = 4;

// Hard ceiling on the page records written. A record is bounded by kPageRecordMaxBytes
// (Section.cpp:104, 16 KiB) so four pages cannot exceed this, and a typical record is 1-4 KiB.
constexpr uint32_t kMaxRecordBytes = kPagesPerAttempt * 16 * 1024;

// Hard ceiling on the spine HTML a prepare may inflate. Above it the inflate is a multi-hundred-ms
// stall on the loop task rather than an idle frame's work, and the same number is the reader's own
// "this chapter will make you wait" threshold (EpubReaderActivity.h:231,
// BUILD_POPUP_BYTE_THRESHOLD). Enforced by spineTooBigForPrefetch().
constexpr uint32_t kMaxSpineBytes = 96 * 1024;

// Twice the idle build's floors (EpubReaderActivity.h:204-205: 32 KiB free, 16 KiB contiguous):
// the prefetch must leave the live path able to start its own build behind it, and its own peak
// is the live chapter open's parser, not a build step's. The exact peak needs glass; this is the
// conservative side of the existing floors rather than a measured number presented as one.
constexpr size_t kMinFreeHeap = 64 * 1024;
constexpr size_t kMinMaxAlloc = 32 * 1024;

// The gate, in one place: the AND of every condition a prepare must hold. Split out so a test can
// prove each field is load-bearing instead of grepping for the first `return false`.
inline bool mayPrepare(const Gates& gates) {
  if (!gates.displayOwned) return false;
  if (!gates.frameBuffer) return false;
  if (gates.overlayOpen) return false;
  if (gates.overlayRefreshPending) return false;
  if (gates.pageBufferStale) return false;
  if (!gates.liveSection) return false;
  if (gates.liveBuilding) return false;
  if (gates.liveNeedsBuild) return false;
  if (gates.scanning) return false;
  if (gates.queuedTurn) return false;
  if (gates.inputDown) return false;
  if (gates.radioHoldsHeap) return false;
  if (gates.freeHeap < kMinFreeHeap) return false;
  if (gates.maxAlloc < kMinMaxAlloc) return false;
  return true;
}

// A spine whose HTML is above the cap is refused before anything is opened: the inflate is the
// one uninterruptible step of a prepare.
inline bool spineTooBigForPrefetch(const size_t spineBytes) { return spineBytes > kMaxSpineBytes; }

// State of the one artefact. It is a file plus these scalars: `ready` means the file is committed
// and no parser is alive, so holding it costs no heap until it is adopted or discarded.
struct State {
  Target target{};
  // SessionGeneration when the prepare started. A change means the reader moved in a way that is
  // not "into the prepared chapter", so the artefact describes a layout nobody will paint.
  uint32_t armedGeneration = 0;
  int pages = 0;
  uint32_t bytes = 0;
  bool ready = false;
  // A prepare that failed or was refused (OOM, a spine above the cap, a card that will not write).
  // Recorded so an idle loop does not retry the same spine every frame; it expires on its own
  // when the reader moves, because the comparison is against the *next* spine.
  int refusedSpine = -1;
};

enum class Action {
  None,     // nothing to do this frame
  Prepare,  // lay out the next chapter's first page
  Adopt,    // the reader has walked into the prepared chapter
  Discard,  // the artefact is stale: drop it (and its file)
};

// The whole lifecycle, as one function: what the next idle frame should do.
//
// The invalidation rule is not a list of navigation call sites -- those are ~30
// section.reset() sites (trace §2) and a list would silently leak one. It is derived: the
// artefact is valid exactly while the chapter it was prepared for is still the chapter the
// reader would reach next, laid out the same way, in the same session. Chapter jump, TOC,
// footnote, search, book close, font/text-settings change and sleep all break one of those four
// facts, and every one of them is checked here rather than at the site that caused it.
inline Action nextAction(const State& state, const Live& live, const Gates& gates) {
  if (state.target.valid()) {
    // The reader is now standing in the chapter this was prepared for. The render task adopts it
    // (the rename is its job, and it runs before any layout); this answer is for the loop task.
    if (state.target.spineIndex == live.spineIndex) return state.ready ? Action::Adopt : Action::Discard;
    if (!state.ready) return Action::Discard;
    if (live.generation != state.armedGeneration) return Action::Discard;
    if (!live.layoutKnown || state.target.layoutSignature != live.layoutSignature) return Action::Discard;
    if (state.target.spineIndex != live.spineIndex + 1) return Action::Discard;
    return Action::None;  // held: valid, waiting for the reader to walk into it
  }
  if (state.refusedSpine == live.spineIndex + 1) return Action::None;
  if (!live.nextSpineExists) return Action::None;
  if (!live.layoutKnown) return Action::None;
  return mayPrepare(gates) ? Action::Prepare : Action::None;
}

// Whether a prepared artefact may be adopted for the chapter the reader is opening.
//
// `landsOnPageZero` is the forward chapter turn (pageTurn sets nextPageNumber = 0). A backward
// turn asks for the chapter's last page (pendingPageJump = UINT16_MAX, EpubReaderActivity.cpp:1665)
// and a first page cannot answer it, so the prepared file is dropped rather than adopted.
inline bool canAdopt(const State& state, const Live& live, const bool landsOnPageZero) {
  if (!state.ready || !state.target.valid()) return false;
  if (!landsOnPageZero) return false;
  if (state.target.spineIndex != live.spineIndex) return false;
  if (!live.layoutKnown || state.target.layoutSignature != live.layoutSignature) return false;
  return true;
}

inline bool withinBudget(const int pages, const uint32_t bytes) {
  return pages > 0 && pages <= kPagesPerAttempt && bytes <= kMaxRecordBytes;
}

}  // namespace chapterprefetch
