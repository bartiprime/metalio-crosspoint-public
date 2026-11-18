#pragma once

#include <cstdint>

#include "ProgressSaves.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// r163 C4 -- the pending-saves store, shared by the render task (which accepts a position after a
// page turn) and the loop task (which flushes it and applies completions).
//
// ProgressSaves holds the rules and is host-tested; this class is only the lock. It is header-only
// and deliberately boring: every method takes the mutex, calls the same-named ProgressSaves method
// and lets go. A recursive mutex, because a completion can arrive from the writer task while the
// loop task is inside a flush.

namespace readerprogress {

class ProgressStore {
 public:
  static ProgressStore& get() {
    static ProgressStore instance;
    return instance;
  }

  bool accept(const char* path, const progressrecord::Record& record, const uint32_t generation,
              const uint32_t nowMs) {
    Lock lock(*this);
    return saves_.accept(path, record, generation, nowMs);
  }

  bool peek(const char* path, progressrecord::Record& out) {
    Lock lock(*this);
    return saves_.peek(path, out);
  }

  bool take(const char* path, ProgressSaves::Handoff& out) {
    Lock lock(*this);
    return saves_.take(path, out);
  }

  void release(const int slot, const uint32_t token) {
    Lock lock(*this);
    saves_.release(slot, token);
  }

  void complete(const int slot, const uint32_t token, const bool written) {
    Lock lock(*this);
    saves_.complete(slot, token, written);
  }

  bool dirty(const char* path) {
    Lock lock(*this);
    return saves_.dirty(path);
  }

  bool inFlight(const char* path) {
    Lock lock(*this);
    return saves_.inFlight(path);
  }

  ProgressSaves::PendingState state(const char* path) {
    Lock lock(*this);
    return saves_.state(path);
  }

  int pendingBooks() {
    Lock lock(*this);
    return saves_.pendingBooks();
  }

  ProgressStore(const ProgressStore&) = delete;
  ProgressStore& operator=(const ProgressStore&) = delete;

 private:
  struct Lock {
    explicit Lock(ProgressStore& store) : store_(store) {
      if (store_.mutex_ != nullptr) xSemaphoreTakeRecursive(store_.mutex_, portMAX_DELAY);
    }
    ~Lock() {
      if (store_.mutex_ != nullptr) xSemaphoreGiveRecursive(store_.mutex_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    ProgressStore& store_;
  };

  ProgressStore() { mutex_ = xSemaphoreCreateRecursiveMutex(); }

  SemaphoreHandle_t mutex_ = nullptr;
  ProgressSaves saves_;
};

}  // namespace readerprogress
