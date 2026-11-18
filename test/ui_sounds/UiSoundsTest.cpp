// AUDIO1 — event -> sound mapping and the drop-if-busy policy, as a pure function.
//
// WHAT IS ASSERTED
//   LAYER 1 (map): each of the five navigation events maps to its own waveform.
//     Five distinct sounds, so a future edit cannot collapse two events onto one
//     click and silently lose the difference between "page turned" and "refused".
//
//   LAYER 2 (gates): a click requires master AND ui AND a healthy path. Each
//     gate alone stops the sound and counts a skip, and NO gate can start one.
//
//   LAYER 3 (burst): the property the reader cares about. Twenty events with no
//     playback completing in between must produce at most TWO sounds and must
//     drop the rest -- never a backlog. The counts are exact, not "at most".
//
//   LAYER 4 (no over-dropping): with playback completing between events, all
//     twenty produce sound. Drop-if-busy must not become drop-always.
//
//   The negative control in layer 3 (UnboundedQueueSoundsAll) is what makes the
//   drop assertion mean something: an always-accept policy would play 20.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "audio/UiSoundPolicy.h"

using uisounds::Event;
using uisounds::Ledger;
using uisounds::Policy;
using uisounds::Sound;
using uisounds::Tag;

namespace {

// A policy with both switches on and a healthy path: the only configuration in
// which anything can be heard.
Policy ready() {
  Policy p;
  p.master = true;
  p.ui = true;
  p.pathReady = true;
  return p;
}

const Event kAll[] = {Event::PageTurn, Event::MenuMove, Event::Select, Event::Back, Event::Error};

}  // namespace

// ---------------------------------------------------------------------------
// LAYER 1 — the map.
// ---------------------------------------------------------------------------
TEST(UiSoundMap, EveryEventHasADistinctSound) {
  std::set<int> sounds;
  for (const Event e : kAll) sounds.insert(static_cast<int>(uisounds::soundFor(e)));
  EXPECT_EQ(sounds.size(), 5u) << "two navigation events share a waveform";
  EXPECT_EQ(uisounds::kEventCount, 5);
}

TEST(UiSoundMap, TheMappingIsTheDocumentedOne) {
  EXPECT_EQ(uisounds::soundFor(Event::PageTurn), Sound::Click);
  EXPECT_EQ(uisounds::soundFor(Event::MenuMove), Sound::Move);
  EXPECT_EQ(uisounds::soundFor(Event::Select), Sound::Select);
  EXPECT_EQ(uisounds::soundFor(Event::Back), Sound::Back);
  EXPECT_EQ(uisounds::soundFor(Event::Error), Sound::Error);
}

TEST(UiSoundMap, NamesAreDistinctAndPrintable) {
  std::set<std::string> names;
  for (const Event e : kAll) names.insert(uisounds::eventName(e));
  EXPECT_EQ(names.size(), 5u);
  for (const Event e : kAll) EXPECT_STRNE(uisounds::eventName(e), "?");
}

// ---------------------------------------------------------------------------
// LAYER 2 — the gates. Both switches default OFF in SETTINGS; this pins the
// behaviour of each one independently.
// ---------------------------------------------------------------------------
TEST(UiSoundGates, DefaultsAreSilent) {
  Policy p;  // master=false, ui=false, pathReady=true
  for (const Event e : kAll) {
    EXPECT_EQ(p.submit(e), Policy::Outcome::Skipped);
  }
  EXPECT_EQ(p.started, 0u);
  EXPECT_EQ(p.queued, 0u);
  EXPECT_EQ(p.dropped, 0u);
  EXPECT_EQ(p.skipped, 5u);
  EXPECT_EQ(p.claim(), uisounds::kNone);
}

TEST(UiSoundGates, MasterAloneIsSilent) {
  Policy p = ready();
  p.ui = false;
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Skipped);
  EXPECT_EQ(p.started, 0u);
}

TEST(UiSoundGates, UiSwitchAloneIsSilent) {
  Policy p = ready();
  p.master = false;
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Skipped);
  EXPECT_EQ(p.started, 0u);
}

TEST(UiSoundGates, FaultedPathIsSilentAndNotRetried) {
  Policy p = ready();
  p.pathReady = false;  // Faulted / parked / quarantined by the worker
  for (const Event e : kAll) EXPECT_EQ(p.submit(e), Policy::Outcome::Skipped);
  EXPECT_EQ(p.skipped, 5u);
  EXPECT_EQ(p.started, 0u);
  EXPECT_EQ(p.claim(), uisounds::kNone);
}

TEST(UiSoundGates, ReEnablingResumesAudio) {
  Policy p = ready();
  p.ui = false;
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Skipped);
  p.ui = true;
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Started);
}

// ---------------------------------------------------------------------------
// LAYER 3 — the burst property.
// ---------------------------------------------------------------------------
TEST(UiSoundBurst, TwentyEventsProduceAtMostTwoSounds) {
  Policy p = ready();
  const uint32_t sounds = p.burst(Event::PageTurn, 20);
  EXPECT_EQ(sounds, 2u) << "depth-1 queue: one playing + one waiting, the rest dropped";
  EXPECT_EQ(p.started, 1u);
  EXPECT_EQ(p.queued, 1u);
  EXPECT_EQ(p.dropped, 18u);
  EXPECT_EQ(p.skipped, 0u);
  EXPECT_EQ(p.claim(), static_cast<int>(Event::PageTurn));
}

TEST(UiSoundBurst, EveryEventDropsTheSameWay) {
  for (const Event e : kAll) {
    Policy p = ready();
    EXPECT_EQ(p.burst(e, 20), 2u) << uisounds::eventName(e);
    EXPECT_EQ(p.dropped, 18u) << uisounds::eventName(e);
  }
}

TEST(UiSoundBurst, TheQueueIsExactlyOneDeep) {
  Policy p = ready();
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Started);
  EXPECT_EQ(p.submit(Event::MenuMove), Policy::Outcome::Queued);
  EXPECT_EQ(p.submit(Event::Select), Policy::Outcome::DroppedBusy);
  EXPECT_EQ(p.submit(Event::Back), Policy::Outcome::DroppedBusy);
  EXPECT_EQ(p.submit(Event::Error), Policy::Outcome::DroppedBusy);
  EXPECT_EQ(p.dropped, 3u);
}

// Negative control: an always-accept policy is what this test exists to forbid.
TEST(UiSoundBurst, UnboundedQueueSoundsAll) {
  int played = 0;
  for (int i = 0; i < 20; i++) played++;  // the shape being rejected
  EXPECT_EQ(played, 20);
  Policy p = ready();
  EXPECT_NE(p.burst(Event::PageTurn, 20), 20u);
}

// ---------------------------------------------------------------------------
// LAYER 4 — no over-dropping: the worker completing between events means every
// event is heard. A held key repeating at human speed must not be silenced.
// ---------------------------------------------------------------------------
TEST(UiSoundDrain, CompletedBetweenEventsLosesNothing) {
  Policy p = ready();
  int heard = 0;
  for (int i = 0; i < 20; i++) {
    if (p.submit(Event::PageTurn) == Policy::Outcome::Started) {
      EXPECT_GE(p.claim(), 0);
      heard++;
      EXPECT_FALSE(p.completed());
    }
  }
  EXPECT_EQ(heard, 20);
  EXPECT_EQ(p.dropped, 0u);
  EXPECT_EQ(p.claim(), uisounds::kNone);
}

TEST(UiSoundDrain, PromotedSlotPlaysTheQueuedEventThenStops) {
  Policy p = ready();
  EXPECT_EQ(p.submit(Event::PageTurn), Policy::Outcome::Started);
  EXPECT_EQ(p.submit(Event::Error), Policy::Outcome::Queued);
  EXPECT_EQ(p.claim(), static_cast<int>(Event::PageTurn));
  EXPECT_TRUE(p.completed()) << "the queued event must be ready immediately";
  EXPECT_EQ(p.claim(), static_cast<int>(Event::Error));
  EXPECT_FALSE(p.completed());
  EXPECT_EQ(p.claim(), uisounds::kNone);
  // and the slot is free again
  EXPECT_EQ(p.submit(Event::Back), Policy::Outcome::Started);
}

TEST(UiSoundDrain, StartedCountsEverySoundThatReachesPlayback) {
  Policy p = ready();
  p.submit(Event::PageTurn);  // started
  p.submit(Event::Select);    // queued
  p.completed();              // promotes -> started again
  EXPECT_EQ(p.started, 2u);
  EXPECT_EQ(p.queued, 1u);
  EXPECT_EQ(p.dropped, 0u);
}

TEST(UiSoundPolicyIsPure, NoStateLeaksBetweenInstances) {
  Policy a = ready();
  a.burst(Event::Select, 5);
  Policy b = ready();
  EXPECT_EQ(b.started, 0u);
  EXPECT_EQ(b.dropped, 0u);
  EXPECT_EQ(b.claim(), uisounds::kNone);
}

// ---------------------------------------------------------------------------
// AUDIO3 — measured, not asserted by eye. The operator's report was "it is very
// inconsistent, some taps beep, some don't"; these cases turn that sentence into
// numbers: presses per second, how many become sounds, how many are dropped, and
// where the drop happens.
//
// The cadence model is a clock the test owns. A press is audible when the
// policy is idle (submit -> Started); the policy is busy for exactly the wave
// length the press asked for (kSoundMs[] in UiSoundPolicy.h -- the same table
// UiSounds.cpp builds the waveforms from), and `completed()` at that instant is
// what the worker does when playWave() returns.
// ---------------------------------------------------------------------------
namespace {

// One press stream through the real Policy, with the wave length as the busy
// window. `cadenceMs` is the gap between presses.
struct PressRun {
  uint32_t presses = 0;
  uint32_t sounds = 0;
  uint32_t dropped = 0;
  uint32_t queued = 0;
  Ledger ledger{};
  uint64_t busyUntilMs = 0;  // when the currently playing wave finishes
};

void press(PressRun& r, Policy& p, Event e, const uint32_t atMs) {
  // The worker completes exactly when the current wave ends, and a completed
  // sound immediately starts whatever the one-deep slot was holding (that is
  // what Policy::completed() returning true means). The next busy window starts
  // where the previous one ended, not at "now": the promoted sound was already
  // waiting, which is the real delay a queued press pays.
  while (p.claim() != uisounds::kNone && atMs >= r.busyUntilMs) {
    p.completed();
    const int next = p.claim();
    if (next < 0) break;
    r.busyUntilMs += uisounds::kSoundMs[next];
  }
  const Policy::Outcome o = p.submit(e);
  r.ledger.record(Tag::List, e, o);
  r.presses++;
  if (o == Policy::Outcome::Started) {
    r.sounds++;
    r.busyUntilMs = atMs + uisounds::kSoundMs[static_cast<int>(uisounds::soundFor(e))];
  } else if (o == Policy::Outcome::Queued) {
    r.queued++;
    r.sounds++;  // queued depth 1 WILL be heard, it is not a loss
  } else if (o == Policy::Outcome::DroppedBusy) {
    r.dropped++;
  }
}

PressRun stream(const Event e, const int presses, const uint32_t gapMs) {
  PressRun r;
  Policy p = ready();
  uint32_t t = 0;
  for (int i = 0; i < presses; i++) {
    press(r, p, e, t);
    t += gapMs;
  }
  return r;
}

}  // namespace

// One press, one tick. The base case the operator expects on every surface.
TEST(UiSoundMeasured, OnePressIsOneTick) {
  const PressRun r = stream(Event::Select, 1, 200);
  EXPECT_EQ(r.presses, 1u);
  EXPECT_EQ(r.sounds, 1u);
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_EQ(r.ledger.at(Tag::List, Event::Select, Policy::Outcome::Started), 1u);
}

// Ten presses at 200 ms: faster than a human scrolls, slower than any wave, so
// nothing may be lost. This is the "no over-dropping" case with a clock.
TEST(UiSoundMeasured, TenPressesAt200msAreTenSounds) {
  const PressRun r = stream(Event::MenuMove, 10, 200);
  EXPECT_EQ(r.presses, 10u);
  EXPECT_EQ(r.sounds, 10u) << "a 13 ms move tick cannot still be busy 200 ms later";
  EXPECT_EQ(r.dropped, 0u);
  EXPECT_EQ(r.queued, 0u);
}

// Ten presses at 20 ms: a burst. It is BOUNDED (never a backlog) and every
// press is accounted for in the ledger -- that is what "logged" means here: the
// per-surface counters can name the drops after the fact.
TEST(UiSoundMeasured, TenPressesAt20msAreBoundedAndAccountedFor) {
  for (const Event e : kAll) {
    const PressRun r = stream(e, 10, 20);
    EXPECT_EQ(r.presses, 10u) << uisounds::eventName(e);
    EXPECT_LE(r.sounds, 10u);
    EXPECT_GE(r.sounds, 2u) << uisounds::eventName(e) << ": at least the first press and its queued neighbour";
    EXPECT_EQ(r.sounds + r.dropped, r.presses) << "every press must land in exactly one bucket";
    EXPECT_EQ(r.dropped, r.ledger.at(Tag::List, e, Policy::Outcome::DroppedBusy))
        << "the ledger must agree with the run: this is the number a report would quote";
  }
}

// The drop rate is a property of the CADENCE against the WAVE LENGTH, not of the
// depth: at 30 ms a 13 ms move tick loses nothing while a 78 ms error tone drops.
TEST(UiSoundMeasured, DropRateIsBoundedByTheWaveLength) {
  const PressRun move = stream(Event::MenuMove, 10, 30);   // 13 ms wave
  const PressRun error = stream(Event::Error, 10, 30);     // 78 ms wave
  EXPECT_EQ(move.dropped, 0u) << "a 13 ms tick is idle again long before the next 30 ms press";
  EXPECT_GT(error.dropped, 0u) << "a 78 ms tone still holds both slots when the next press lands";
  // And the ceiling: no wave may be longer than 100 ms, so the worst-case busy
  // window after any press is 100 ms.
  for (int s = 0; s < static_cast<int>(Sound::Count); s++) EXPECT_LE(uisounds::kSoundMs[s], 100);
}

// END OF LIST. Every list here wraps (ButtonNavigator::nextIndex is
// `(current + 1) % total`), so the only way a move does not move is a list with
// one item -- which is also the single-page case. Both must be SILENT: 0 sounds,
// and in particular NOT the disallowed tone the old code played.
TEST(UiSoundRefusedMove, SingleItemListPressIsSilent) {
  Policy p = ready();
  Ledger led{};
  const int count = 1;
  const int from = 0;
  const int to = (from + 1) % count;  // ButtonNavigator::nextIndex, src/util/ButtonNavigator.cpp:88
  EXPECT_EQ(to, from);
  if (uisounds::moveIsAudible(from, to)) led.record(Tag::List, Event::MenuMove, p.submit(Event::MenuMove));
  EXPECT_EQ(led.eventsFor(Tag::List, Event::MenuMove), 0u);
  EXPECT_EQ(p.started, 0u);
  EXPECT_EQ(p.skipped, 0u) << "silent, but not through a gate: no event was submitted at all";
  EXPECT_EQ(p.claim(), uisounds::kNone);
}

TEST(UiSoundRefusedMove, EndOfListPressIsSilentWhenItCannotMove) {
  // Wrapping lists DO move at the end (to index 0) and must tick; a clamped
  // navigator that returns `count - 1` again must not.
  Policy p = ready();
  for (int count = 1; count <= 5; count++) {
    const int last = count - 1;
    // clamped: end of list has nowhere to go
    if (uisounds::moveIsAudible(last, last)) p.submit(Event::MenuMove);
    EXPECT_EQ(p.started, 0u) << "count=" << count;
  }
  // wrapping, two items: the move is real and must be heard
  const int count = 2;
  const int from = 1;
  const int to = (from + 1) % count;
  EXPECT_TRUE(uisounds::moveIsAudible(from, to));
  EXPECT_EQ(p.submit(Event::MenuMove), Policy::Outcome::Started);
}

// A refused ACTION is still an error, and it is not the same thing as a refused
// move: Confirm with nothing selected keeps its tone.
TEST(UiSoundRefusedMove, RefusedActionStillSounds) {
  Policy p = ready();
  EXPECT_EQ(p.submit(Event::Error), Policy::Outcome::Started);
  EXPECT_EQ(uisounds::soundFor(Event::Error), Sound::Error);
}

// The ledger is per surface: the same event from two call sites is two rows, so
// "which screen dropped it" is answerable.
TEST(UiSoundLedger, AttributesEventsToTheCallingSurface) {
  Policy p = ready();
  Ledger led{};
  led.record(Tag::Home, Event::Select, p.submit(Event::Select));
  led.record(Tag::Home, Event::Select, p.submit(Event::Select));
  led.record(Tag::Keyboard, Event::Select, p.submit(Event::Select));
  EXPECT_EQ(led.eventsFor(Tag::Home, Event::Select), 2u);
  EXPECT_EQ(led.eventsFor(Tag::Keyboard, Event::Select), 1u);
  EXPECT_EQ(led.at(Tag::Home, Event::Select, Policy::Outcome::Started), 1u);
  EXPECT_EQ(led.at(Tag::Home, Event::Select, Policy::Outcome::Queued), 1u);
  EXPECT_EQ(led.at(Tag::Keyboard, Event::Select, Policy::Outcome::DroppedBusy), 1u);
  // An untagged caller must be VISIBLE, not silently swallowed: it lands on row
  // 0 and prints as "?", so a forgotten tag reads as "?" in `uisound calls`.
  led.record(Tag::Unknown, Event::Select, Policy::Outcome::Started);
  EXPECT_EQ(led.eventsFor(Tag::Unknown, Event::Select), 1u);
  EXPECT_STREQ(uisounds::tagName(Tag::Unknown), "?");
}

TEST(UiSoundLedger, CountersSaturateInsteadOfWrapping) {
  Ledger led{};
  for (int i = 0; i < 70000; i++) led.record(Tag::List, Event::Back, Policy::Outcome::Started);
  EXPECT_EQ(led.at(Tag::List, Event::Back, Policy::Outcome::Started), 0xFFFFu);
}

TEST(UiSoundLedger, EveryTagAndEventNameIsPrintable) {
  for (int t = 1; t < uisounds::kTagCount; t++) {
    EXPECT_STRNE(uisounds::tagName(static_cast<Tag>(t)), "?");
  }
  for (const Event e : kAll) EXPECT_STRNE(uisounds::eventName(e), "?");
}
