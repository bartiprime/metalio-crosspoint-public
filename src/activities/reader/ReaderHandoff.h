#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "activities/RenderLock.h"

// r163 C3 -- the reader -> UI handoff contract.
//
// The vendor firmware documents the same four rules (docs/ui-scheduling.md:9,74-76); this file is
// the reader's copy of them as types rather than as a comment somebody can ignore:
//
//  1. A worker callback never waits on another worker, touches the card, rebuilds a chapter or
//     rasterises. The only cross-task producer the reader has today is the progress writer
//     (ProgressWriter.h), whose whole job is a 10-byte write; anything that needs the card, a
//     Section or the renderer stays on the task that owns those (the loop task, or the render
//     task under RenderLock).
//  2. A result owns its data and carries a session generation. CompletionSlots stores the payload
//     by value (trivially copyable, bounded size), so there is no pointer back into worker state,
//     and poll() drops anything whose generation is no longer live instead of applying it.
//  3. Completion is advertised through fixed slots the UI polls, not through a callback queue
//     whose capacity can silently swallow a completion. publish() answers false when every slot
//     is occupied: a refusal the producer can see, retry or report -- never a silent overwrite.
//  4. DisplayLockGuard acquisition is Try-only, and there is no way to ask it to block. owns() ==
//     false means the caller returns immediately; the guard is inert, and only the acquirer
//     unlocks (the destructor unlocks only what it took).

namespace readerhandoff {

// What a prepared result describes. Two results with equal identities are interchangeable.
struct SessionIdentity {
  const void* section = nullptr;
  int spineIndex = -1;
  uint32_t layoutSignature = 0;

  bool operator==(const SessionIdentity& other) const {
    return section == other.section && spineIndex == other.spineIndex && layoutSignature == other.layoutSignature;
  }
  bool operator!=(const SessionIdentity& other) const { return !(*this == other); }
};

// The session a prepared result belongs to. Bumped when -- and only when -- the identity of the
// work changes: a new Section, a new spine, or a layout that invalidates the pinned page grid.
// A repaint of the same page in the same chapter is NOT a new session, so idle work prepared
// across a status-bar repaint survives it.
//
// observe() runs on the render task (renderBook); current()/live() are read from the loop task,
// which is why the counter is atomic and the identity is only ever touched by the observer.
class SessionGeneration {
 public:
  uint32_t current() const { return generation_.load(std::memory_order_acquire); }

  // Returns true when the identity changed and the generation was bumped.
  bool observe(const SessionIdentity& identity) {
    if (observed_ && identity == identity_) return false;
    identity_ = identity;
    observed_ = true;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  bool live(const uint32_t generation) const { return generation == current(); }

 private:
  std::atomic<uint32_t> generation_{1};
  SessionIdentity identity_{};
  bool observed_ = false;
};

// Fixed completion slots: one producer (a worker), one consumer (the UI). No allocation, no
// locking, and no callback -- the consumer polls. Payloads are values: the producer publishes a
// copy and is then free to reuse or destroy its own.
template <typename Payload, int N>
class CompletionSlots {
  static_assert(N > 0, "a slot set needs at least one slot");
  static_assert(std::is_trivially_copyable_v<Payload>, "a result must own its data: no pointers into worker state");
  static_assert(sizeof(Payload) <= 64, "a result is a value, not a buffer");

 public:
  // False when every slot is already occupied: the producer must treat that as a refusal (keep its
  // own state, retry later, or report), never as a delivered result.
  bool publish(const uint32_t generation, const Payload& payload) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= static_cast<uint32_t>(N)) return false;
    Slot& slot = slots_[head % static_cast<uint32_t>(N)];
    slot.generation = generation;
    slot.payload = payload;
    slot.ready.store(1, std::memory_order_release);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // The oldest result that belongs to `liveGeneration`, or false. Results from any other
  // generation are dropped here -- not returned, not applied.
  bool poll(const uint32_t liveGeneration, Payload& out) {
    for (;;) {
      const uint32_t tail = tail_.load(std::memory_order_relaxed);
      if (tail == head_.load(std::memory_order_acquire)) return false;
      Slot& slot = slots_[tail % static_cast<uint32_t>(N)];
      if (slot.ready.load(std::memory_order_acquire) == 0) return false;
      const bool live = slot.generation == liveGeneration;
      if (live) out = slot.payload;
      slot.ready.store(0, std::memory_order_release);
      tail_.store(tail + 1, std::memory_order_release);
      if (live) return true;
    }
  }

  // Results published and not yet polled, including ones a later poll will drop as stale.
  int occupied() const { return static_cast<int>(head_.load(std::memory_order_acquire) -
                                                 tail_.load(std::memory_order_acquire)); }

 private:
  struct Slot {
    std::atomic<uint32_t> ready{0};
    uint32_t generation = 0;
    Payload payload{};
  };
  Slot slots_[N];
  std::atomic<uint32_t> head_{0};  // producer
  std::atomic<uint32_t> tail_{0};  // consumer
};

// The reader's display guard. Rule 4 as a type: the only constructor acquires in Try mode, so a
// caller cannot stall the UI on a compose by accident, and owns() is [[nodiscard]] so a site that
// acquires and then uses the display without checking does not compile quietly.
class DisplayLockGuard {
 public:
  DisplayLockGuard() : lock_(RenderLock::Mode::Try) {}
  ~DisplayLockGuard() = default;
  DisplayLockGuard(const DisplayLockGuard&) = delete;
  DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;
  DisplayLockGuard(DisplayLockGuard&&) = delete;
  DisplayLockGuard& operator=(DisplayLockGuard&&) = delete;

  [[nodiscard]] bool owns() const { return lock_.ownsLock(); }

 private:
  RenderLock lock_;
};

}  // namespace readerhandoff
