#pragma once

#include <cstdint>
#include <cstring>

#include "ProgressRecord.h"

// r163 C4 -- coalesced progress saves.
//
// The vendor's rule (docs/ui-scheduling.md:31,88): a pending save for a book MERGES to the latest
// position instead of queueing, a reopen consults the unflushed snapshot, and a destructor never
// saves a stale position. Read literally, that is about what a save *is*: an accepted snapshot of
// where the reader is, not a job that will write that position when it gets there.
//
// So there is one record per book here, never a queue:
//   * accept() replaces the book's pending record and gives it a new sequence, so ten page turns
//     before the write lands cost one write of the tenth position, not ten writes;
//   * peek() hands back the pending record, which is what a reopen must read: it is newer than the
//     file on the card by definition;
//   * take() hands the record to a writer with the snapshot's token, and complete() only marks the
//     pending clean for the token that was actually written -- a newer accept during the write
//     leaves the book dirty, so it is written again.
//
// Pure and host-tested (test/progress_saves). ProgressStore.h adds the mutex that makes it usable
// from the render task and the loop task at once.

namespace readerprogress {

class ProgressSaves {
 public:
  static constexpr int kBooks = 4;
  static constexpr size_t kPathMax = 72;

  // Returns false only when every slot belongs to another book whose position is not on the card
  // yet -- accepted but unwritten, or a write in flight. A false here is a refusal the caller can
  // see; it never silently drops a position, and it never evicts a dirty one to make room.
  bool accept(const char* path, const progressrecord::Record& record, const uint32_t generation,
              uint32_t nowMs) {
    Slot* slot = find(path);
    if (slot == nullptr) {
      slot = freeSlot();
      if (slot == nullptr) slot = evictOldestClean();
      if (slot == nullptr) return false;
      clear(*slot);
      copyPath(*slot, path);
    }
    slot->record = record;
    slot->generation = generation;
    slot->seq = ++seqs_;
    slot->lastUsedMs = nowMs;
    return true;
  }

  // The unflushed snapshot for this book, if there is one. This is what a reopen reads first.
  bool peek(const char* path, progressrecord::Record& out) const {
    const Slot* slot = find(path);
    if (slot == nullptr || !isDirty(*slot)) return false;
    out = slot->record;
    return true;
  }

  // What a writer is handed: the snapshot, the token that names it, and the slot its completion
  // must be reported against.
  struct Handoff {
    progressrecord::Record record{};
    uint32_t token = 0;
    uint32_t generation = 0;
    int slot = -1;
  };

  // Hand the pending snapshot to a writer. False when nothing is pending, or a write of the same
  // book is already in flight (one writer per book, so two writers cannot race on the same temp
  // file).
  bool take(const char* path, Handoff& out) {
    Slot* slot = find(path);
    if (slot == nullptr || !isDirty(*slot) || slot->inFlight) return false;
    slot->inFlight = true;
    slot->token = slot->seq;
    out.record = slot->record;
    out.token = slot->token;
    out.generation = slot->generation;
    out.slot = static_cast<int>(slot - slots_);
    return true;
  }

  // Give the snapshot back without writing it (the writer refused the job): still pending.
  void release(const int slotIndex, const uint32_t token) {
    Slot* slot = slotForToken(slotIndex, token);
    if (slot != nullptr) slot->inFlight = false;
  }

  // A write finished. The pending mark clears only for the snapshot that was written: a newer
  // accept during the write leaves the book dirty, so the newer position is written next.
  void complete(const int slotIndex, const uint32_t token, const bool written) {
    Slot* slot = slotForToken(slotIndex, token);
    if (slot == nullptr) return;
    slot->inFlight = false;
    if (written) slot->writtenToken = token;
  }

  // Still not on the card: accepted but not written, or a write in flight.
  bool dirty(const char* path) const {
    const Slot* slot = find(path);
    return slot != nullptr && isDirty(*slot);
  }

  bool inFlight(const char* path) const {
    const Slot* slot = find(path);
    return slot != nullptr && slot->inFlight;
  }

  // Both answers under ONE lock. A caller that acts on "pending and nobody is writing it" must not
  // have to ask twice: between two questions the writer can finish, and the pair it then believes
  // never existed. Pure; the owner's mutex carries this across tasks.
  struct PendingState {
    bool dirty = false;
    bool inFlight = false;
  };

  PendingState state(const char* path) const {
    const Slot* slot = find(path);
    if (slot == nullptr) return {};
    return {isDirty(*slot), slot->inFlight};
  }

  int pendingBooks() const {
    int count = 0;
    for (const auto& slot : slots_) count += (slot.used && isDirty(slot)) ? 1 : 0;
    return count;
  }

 private:
  struct Slot {
    bool used = false;
    char path[kPathMax] = {};
    progressrecord::Record record{};
    uint32_t generation = 0;
    uint32_t seq = 0;           // identity of the snapshot: bumped by every accept, never reused
    uint32_t writtenToken = 0;  // the snapshot that is on the card
    uint32_t token = 0;         // the snapshot handed to the writer in flight
    uint32_t lastUsedMs = 0;
    bool inFlight = false;
  };

  static bool isDirty(const Slot& slot) { return slot.inFlight || slot.seq != slot.writtenToken; }

  static void clear(Slot& slot) { slot = Slot{}; }

  static void copyPath(Slot& slot, const char* path) {
    std::strncpy(slot.path, path, kPathMax - 1);
    slot.path[kPathMax - 1] = '\0';
    slot.used = true;
  }

  Slot* find(const char* path) {
    for (auto& slot : slots_) {
      if (slot.used && std::strncmp(slot.path, path, kPathMax) == 0) return &slot;
    }
    return nullptr;
  }

  const Slot* find(const char* path) const {
    for (const auto& slot : slots_) {
      if (slot.used && std::strncmp(slot.path, path, kPathMax) == 0) return &slot;
    }
    return nullptr;
  }

  Slot* slotForToken(const int slotIndex, const uint32_t token) {
    if (slotIndex < 0 || slotIndex >= kBooks) return nullptr;
    Slot& slot = slots_[slotIndex];
    // A token from a write whose slot has since been reused belongs to another book now.
    if (!slot.used || slot.token != token) return nullptr;
    return &slot;
  }

  Slot* freeSlot() {
    for (auto& slot : slots_) {
      if (!slot.used) return &slot;
    }
    return nullptr;
  }

  // Every slot is taken: reuse the quietest one whose position is already ON THE CARD. Never one
  // whose write is running (that would strand the write and lose its result) and never one that is
  // accepted-but-unwritten (that would drop a position the reader was told was remembered).
  Slot* evictOldestClean() {
    Slot* oldest = nullptr;
    for (auto& slot : slots_) {
      if (!slot.used || isDirty(slot)) continue;
      if (oldest == nullptr || slot.lastUsedMs < oldest->lastUsedMs) oldest = &slot;
    }
    return oldest;
  }

  Slot slots_[kBooks];
  uint32_t seqs_ = 0;
};

}  // namespace readerprogress
