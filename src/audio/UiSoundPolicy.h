#pragma once

// ---------------------------------------------------------------------------
// AUDIO1 — the UI-sound policy, as a PURE function of state.
//
// Split out of UiSounds.cpp on purpose: this header has no Arduino, no FreeRTOS
// and no btaudio dependency, so the exact object the firmware runs is compiled
// and driven on the host (test/ui_sounds). The device wrapper (UiSounds.cpp)
// owns the spinlock, the worker task and the waveform tables and calls INTO
// this; it does not re-implement any of the decisions below.
//
// WHAT IT DECIDES
//   * which events make a sound at all (the two persisted gates + path health)
//   * whether the event starts a sound, waits in the ONE-deep queue, or is
//     dropped because the player is already busy
//
// WHY THE QUEUE IS ONE DEEP
//   A click that delays a paint is worse than silence, and a burst (a held key,
//   auto-repeat, a tilt that fires repeatedly) must not turn into a backlog that
//   trails the UI by hundreds of milliseconds. Depth 1 = at most one sound
//   playing plus one waiting. Everything after that is dropped, not queued, and
//   counted, so a stuck path is visible instead of hidden.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace uisounds {

enum class Event : uint8_t { PageTurn = 0, MenuMove, Select, Back, Error, Count };
constexpr int kEventCount = static_cast<int>(Event::Count);
constexpr int8_t kNone = -1;

// WHERE an event came from. AUDIO3: the operator's report was "inconsistent --
// some taps beep, some don't", and that sentence is what a counter table alone
// cannot answer: a dropped event is only actionable once you know which surface
// dropped it. Every onEvent() caller names itself with one of these, the ledger
// below keeps a count per (tag, event, outcome), and the debug shell can print
// the table -- so a future report can say "the Home grid dropped 14 selects",
// never "it is inconsistent".
enum class Tag : uint8_t {
  Unknown = 0,
  List,        // UiListActivity (settings, library, catalog, menus built on it)
  TabList,     // UiTabListActivity (tabbed lists)
  PageTurn,    // EpubReaderActivity pageTurn()
  ReaderMenu,  // EpubReaderMenuActivity: the in-book More menu's own buttons
  Home,        // HomeActivity cover grid / recent list
  Dialog,      // OptionPopup (and every dialog built on it)
  Keyboard,    // KeyboardEntryActivity
  Back,        // ActivityManager: the global Back that leaves an activity
  Count
};
constexpr int kTagCount = static_cast<int>(Tag::Count);

inline const char* tagName(Tag t) {
  switch (t) {
    case Tag::List: return "list";
    case Tag::TabList: return "tablist";
    case Tag::PageTurn: return "pageturn";
    case Tag::ReaderMenu: return "readermenu";
    case Tag::Home: return "home";
    case Tag::Dialog: return "dialog";
    case Tag::Keyboard: return "keyboard";
    case Tag::Back: return "back";
    default: return "?";
  }
}

// One waveform per distinct user-visible meaning. `Click` and `Move` differ in
// length and pitch so a page turn and a selection move cannot be confused.
enum class Sound : uint8_t { Click = 0, Move, Select, Back, Error, Count };

// AUDIO3: THE BUSY WINDOW IS THE WAVE, and both sides read it from HERE.
// UiSounds.cpp builds the waveforms from this table (a static_assert there
// pins it to the spec array), and the host test models a press cadence against
// the same numbers -- so "how long can the policy be busy after a press" is
// answerable without a device, and a spec edit cannot desync the two.
constexpr uint16_t kSoundMs[static_cast<int>(Sound::Count)] = {
    /* Click */ 22, /* Move */ 13, /* Select */ 44, /* Back */ 40, /* Error */ 78};

inline const char* eventName(Event e) {
  switch (e) {
    case Event::PageTurn: return "pageTurn";
    case Event::MenuMove: return "menuMove";
    case Event::Select: return "select";
    case Event::Back: return "back";
    case Event::Error: return "error";
    default: return "?";
  }
}

inline const char* soundName(Sound s) {
  switch (s) {
    case Sound::Click: return "click";
    case Sound::Move: return "move";
    case Sound::Select: return "select";
    case Sound::Back: return "back";
    case Sound::Error: return "error";
    default: return "?";
  }
}

// The event -> waveform map. All five are distinct: this is the property the
// host test asserts, so a future edit cannot quietly collapse two events onto
// one sound.
inline Sound soundFor(Event e) {
  switch (e) {
    case Event::PageTurn: return Sound::Click;
    case Event::MenuMove: return Sound::Move;
    case Event::Select: return Sound::Select;
    case Event::Back: return Sound::Back;
    case Event::Error: return Sound::Error;
    default: return Sound::Click;
  }
}

// AUDIO3 — is a selection move audible?
//
// The rule the operator asked for: a press that does not move the selection is
// SILENT, not an error. It fires at the end of a list and on a one-page list,
// which are both the same condition -- `to == from`. (Every list in this UI
// wraps: ButtonNavigator::nextIndex is `(current + 1) % total`, so on anything
// longer than one item the selection really does move and a tick is right.)
//
// It lives here, next to the policy, so UiListActivity, UiTabListActivity and
// the host test all read ONE predicate instead of three copies of `!=`.
inline bool moveIsAudible(const int from, const int to) { return to >= 0 && to != from; }

struct Policy {
  // --- gates (set by the device wrapper; never read from hardware in here) ---
  bool master = false;     // SETTINGS.audioOutEnabled   (default 0)
  bool ui = false;         // SETTINGS.uiSoundsEnabled   (default 0)
  bool pathReady = true;   // worker has not quarantined the path (faulted/parked)

  // --- state ---
  int8_t current = kNone;  // committed to play; cleared by completed()
  int8_t pending = kNone;  // the single queue slot

  // --- counters (the device prints these; the host test asserts on them) ---
  uint32_t started = 0;
  uint32_t queued = 0;
  uint32_t dropped = 0;
  uint32_t skipped = 0;  // gate off / path quarantined / bad event

  enum class Outcome : uint8_t { Started, Queued, DroppedBusy, Skipped };

  Outcome submit(Event e) {
    if (e >= Event::Count) {
      skipped++;
      return Outcome::Skipped;
    }
    if (!master || !ui || !pathReady) {
      skipped++;
      return Outcome::Skipped;
    }
    const int8_t idx = static_cast<int8_t>(e);
    if (current == kNone) {
      current = idx;
      started++;
      return Outcome::Started;
    }
    if (pending == kNone) {
      pending = idx;
      queued++;
      return Outcome::Queued;
    }
    dropped++;
    return Outcome::DroppedBusy;
  }

  // Worker: the event to play now, or kNone.
  int claim() const { return current; }

  // Worker: playback finished. Promotes the queue slot; true when another sound
  // is ready to play immediately (so the worker must not wait on the semaphore).
  bool completed() {
    if (pending != kNone) {
      current = pending;
      pending = kNone;
      started++;
      return true;
    }
    current = kNone;
    return false;
  }

  // A burst of `n` copies of one event, nothing completing in between.
  // Returns how many actually became sounds (<= 2 by construction).
  uint32_t burst(Event e, uint32_t n) {
    uint32_t sounds = 0;
    for (uint32_t i = 0; i < n; i++) {
      const Outcome o = submit(e);
      if (o == Outcome::Started || o == Outcome::Queued) sounds++;
    }
    return sounds;
  }
};

// AUDIO3 — the per-call-site ledger. Pure data plus a saturating increment, so
// it is the same object on the device (written under the wrapper's spinlock) and
// in the host test. It is a LEDGER, not a policy: nothing here decides anything,
// which is why the instruments cannot silently change what the reader hears.
struct Ledger {
  // [tag][event][outcome] — outcome order matches Outcome below.
  uint16_t cell[kTagCount][kEventCount][4];

  static int indexOf(Policy::Outcome o) {
    switch (o) {
      case Policy::Outcome::Started: return 0;
      case Policy::Outcome::Queued: return 1;
      case Policy::Outcome::DroppedBusy: return 2;
      default: return 3;  // Skipped
    }
  }

  static const char* outcomeName(int i) {
    switch (i) {
      case 0: return "started";
      case 1: return "queued";
      case 2: return "dropped";
      case 3: return "skipped";
      default: return "?";
    }
  }

  // Saturating: a counter that wraps would report "0 drops" after 65536 of them,
  // which is the one reading this instrument exists to prevent.
  //
  // Tag::Unknown is KEPT, on row 0, and prints as "?": a caller that forgot its
  // tag has to be visible as a "?" row. Dropping it would make an untagged call
  // site look like a surface that never fired -- the exact blindness this ledger
  // exists to remove.
  void record(Tag t, Event e, Policy::Outcome o) {
    const int ti = static_cast<int>(t);
    const int ei = static_cast<int>(e);
    if (ti < 0 || ti >= kTagCount || ei < 0 || ei >= kEventCount) return;
    uint16_t& c = cell[ti][ei][indexOf(o)];
    if (c != 0xFFFF) c++;
  }

  uint16_t at(Tag t, Event e, Policy::Outcome o) const {
    return cell[static_cast<int>(t)][static_cast<int>(e)][indexOf(o)];
  }

  // Every outcome for one (tag, event), summed: the "how many presses did this
  // surface make" number a report quotes.
  uint32_t eventsFor(Tag t, Event e) const {
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) n += cell[static_cast<int>(t)][static_cast<int>(e)][i];
    return n;
  }
};

}  // namespace uisounds
